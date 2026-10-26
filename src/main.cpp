#include <Geode/Geode.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/LevelEditorLayer.hpp>
#include <Geode/binding/SimplePlayer.hpp>
#include <Geode/ui/TextInput.hpp>
#include <Geode/utils/web.hpp>
#include <matjson.hpp>
#include <chrono>
#include <algorithm>
#include <cctype>
#include <random>
#include <map>
#include <string>
#include <vector>

using namespace geode::prelude;

namespace {
    constexpr char kDatabaseUrl[] = "https://gd-rtc-default-rtdb.firebaseio.com";
    constexpr char kAlphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    std::string g_activeRoomCode;
    std::string g_activePeerId;
    bool g_refreshPending = false;
    bool g_roomOperationPending = false;
    bool g_stateSendPending = false;
    bool g_statePollPending = false;
    std::map<std::string, CCNode*> g_remoteAvatars;
    std::vector<std::string> g_roomMemberIds;
    std::vector<std::pair<std::string, std::string>> g_savedLevelsCache;

    double nowSeconds() {
        return std::chrono::duration<double>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    void cacheSharedLevel(std::string const& name, std::string const& data) {
        if (data.empty() || data.size() > 950000) return;
        auto count = Mod::get()->getSavedValue<int>("saved-level-count", 0);
        if (count < 0) count = 0;
        if (count > 8) count = 8;
        int existing = -1;
        for (int i = 0; i < count; ++i) {
            auto savedData = Mod::get()->getSavedValue<std::string>("saved-level-data-" + std::to_string(i), "");
            if (savedData == data) { existing = i; break; }
        }
        if (existing < 0) {
            if (count == 8) {
                for (int i = 1; i < 8; ++i) {
                    auto oldName = Mod::get()->getSavedValue<std::string>("saved-level-name-" + std::to_string(i), "");
                    auto oldData = Mod::get()->getSavedValue<std::string>("saved-level-data-" + std::to_string(i), "");
                    Mod::get()->setSavedValue<std::string>("saved-level-name-" + std::to_string(i - 1), oldName);
                    Mod::get()->setSavedValue<std::string>("saved-level-data-" + std::to_string(i - 1), oldData);
                }
                count = 7;
            }
            existing = count++;
            Mod::get()->setSavedValue<int>("saved-level-count", count);
        }
        Mod::get()->setSavedValue<std::string>("saved-level-name-" + std::to_string(existing), name);
        Mod::get()->setSavedValue<std::string>("saved-level-data-" + std::to_string(existing), data);
        Mod::get()->setSavedValue<std::string>("saved-level-name", name);
        Mod::get()->setSavedValue<std::string>("saved-level-data", data);
    }

    void openLevelEditor(std::string const& name, std::string const& data) {
        if (data.empty()) return;
        auto level = GJGameLevel::create();
        level->m_levelName = name.c_str();
        level->m_levelString = data.c_str();
        level->m_levelID = 0;
        auto scene = LevelEditorLayer::scene(level, false);
        if (scene) CCDirector::sharedDirector()->pushScene(scene);
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

    void setMenuTouchRecursive(CCNode* node, bool enabled, CCNode* skip) {
        if (!node || node == skip) return;
        if (auto menu = typeinfo_cast<CCMenu*>(node)) menu->setTouchEnabled(enabled);
        auto children = node->getChildren();
        if (!children) return;
        for (unsigned int i = 0; i < children->count(); ++i) {
            setMenuTouchRecursive(typeinfo_cast<CCNode*>(children->objectAtIndex(i)), enabled, skip);
        }
    }

    void setBackgroundMenusEnabled(bool enabled) {
        setMenuTouchRecursive(this, enabled, this->getChildByID("gd-rtc-room-panel"));
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
        membersBack->setPosition({-155.f, -32.f});
        panel->addChild(membersBack, 0);

        auto members = CCLabelBMFont::create("Players in room:", "goldFont.fnt");
        members->setID("gd-rtc-room-members");
        members->setScale(0.42f);
        members->setPosition({0, 59.f});
        panel->addChild(members);

        auto memberMenu = CCMenu::create();
        memberMenu->setID("gd-rtc-member-menu");
        memberMenu->setPosition({0, 0});
        panel->addChild(memberMenu);

        auto status = CCLabelBMFont::create("Ready", "goldFont.fnt");
        status->setID("gd-rtc-room-status");
        status->setScale(0.42f);
        status->setPosition({0, -178.f});
        panel->addChild(status);

        auto menu = CCMenu::create();
        menu->setID("gd-rtc-room-panel-menu");
        menu->setPosition({0, -70.f});
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
        closeBtn->setPosition({0, -80});
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
                g_roomMemberIds.clear();
                if (auto panel = self->getChildByID("gd-rtc-room-panel")) {
                    if (auto menu = typeinfo_cast<CCMenu*>(panel->getChildByID("gd-rtc-member-menu"))) menu->removeAllChildren();
                }
                for (auto const& [id, value] : members) {
                    if (id == g_activePeerId) continue;
                    if (count >= 5) break;
                    auto result = value["name"].asString();
                    std::string name = result ? result.unwrap() : "Player";
                    if (name.size() > 16) name.resize(16);
                    auto state = value["state"];
                    auto level = state["levelName"].asString();
                    auto percent = state["percent"].asDouble();
                    auto updated = state["updatedAt"].asDouble();
                    std::string row = name;
                    if (level && updated && nowSeconds() - updated.unwrap() < 3.0) {
                        std::string levelName = level.unwrap();
                        if (levelName.size() > 14) levelName.resize(14);
                        row += " — " + levelName;
                        if (percent) row += " " + std::to_string(static_cast<int>(percent.unwrap())) + "%";
                    } else {
                        row += " — waiting for level";
                    }
                    g_roomMemberIds.push_back(id);
                    if (auto panel = self->getChildByID("gd-rtc-room-panel")) {
                        if (auto menu = typeinfo_cast<CCMenu*>(panel->getChildByID("gd-rtc-member-menu"))) {
                            auto button = CCMenuItemSpriteExtra::create(
                                ButtonSprite::create(row.c_str(), 275, true, "bigFont.fnt", "GJ_button_01.png", 20.f, .38f),
                                self, menu_selector(GDRTCMenuLayer::onGoToPlayerLevel));
                            button->setTag(static_cast<int>(g_roomMemberIds.size() - 1));
                            button->setPosition({0.f, 39.f - static_cast<float>(count) * 20.f});
                            menu->addChild(button);
                        }
                    }
                    ++count;
                }
                if (!count) names += "\n—";
                self->setRoomMembers(names);
                if (g_activeRoomCode == code) self->setRoomStatus("Room " + code + " · " + std::to_string(count + 1) + "/8 players");
            });
    }

    void onGoToPlayerLevel(CCObject* sender) {
        auto item = typeinfo_cast<CCMenuItemSpriteExtra*>(sender);
        if (!item || g_activeRoomCode.empty()) return;
        auto index = item->getTag();
        if (index < 0 || static_cast<size_t>(index) >= g_roomMemberIds.size()) return;
        auto code = g_activeRoomCode;
        auto peer = g_roomMemberIds[static_cast<size_t>(index)];
        setRoomStatus("Loading that player's level...");
        Ref<GDRTCMenuLayer> self = this;
        async::spawn(geode::utils::web::WebRequest()
            .timeout(std::chrono::seconds(12))
            .get(roomUrl(code, "/members/" + peer + "/level")),
            [self, code](geode::utils::web::WebResponse response) {
                if (g_activeRoomCode != code) return;
                if (!response.ok()) {
                    self->setRoomStatus("Couldn't load that player's level yet.");
                    return;
                }
                auto parsed = response.json();
                if (!parsed || parsed.unwrap().isNull()) {
                    self->setRoomStatus("That player hasn't shared level data yet.");
                    return;
                }
                auto nameValue = parsed.unwrap()["name"].asString();
                auto dataValue = parsed.unwrap()["levelData"].asString();
                if (!dataValue || dataValue.unwrap().empty()) {
                    self->setRoomStatus("That player's level data isn't ready yet.");
                    return;
                }
                std::string name = nameValue ? nameValue.unwrap() : "Shared level";
                std::string data = dataValue.unwrap();
                cacheSharedLevel(name, data);
                auto level = GJGameLevel::create();
                level->m_levelName = name.c_str();
                level->m_levelString = data.c_str();
                level->m_levelID = 0;
                auto scene = PlayLayer::scene(level, false, false);
                if (!scene) scene = LevelEditorLayer::scene(level, false);
                if (scene) CCDirector::sharedDirector()->pushScene(scene);
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



class $modify(GDRTCPlayLayer, PlayLayer) {
    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);
        if (g_activeRoomCode.empty() || !this->m_player1 || !this->m_level) return;

        static double lastSendAt = 0.0;
        static double lastPollAt = 0.0;
        auto now = nowSeconds();
        auto code = g_activeRoomCode;
        auto peer = g_activePeerId;

        // Publish the current level once per room/peer/level so room members can
        // use the per-player Go button without uploading level data every frame.
        static std::string lastPublishedLevelKey;
        auto levelName = std::string(this->m_level->m_levelName.c_str());
        auto levelData = std::string(this->m_level->m_levelString.c_str());
        auto levelKey = code + "/" + peer + "/" + std::to_string(static_cast<int>(this->m_level->m_levelID)) + "/" + levelName;
        if (levelKey != lastPublishedLevelKey && !levelData.empty() && levelData.size() < 950000) {
            lastPublishedLevelKey = levelKey;
            matjson::Value levelInfo = matjson::Value::object();
            levelInfo["name"] = levelName;
            levelInfo["levelData"] = levelData;
            levelInfo["levelID"] = static_cast<int>(this->m_level->m_levelID);
            levelInfo["updatedAt"] = now;
            async::spawn(geode::utils::web::WebRequest()
                .timeout(std::chrono::seconds(12))
                .bodyJSON(levelInfo)
                .put(roomUrl(code, "/members/" + peer + "/level")),
                [](geode::utils::web::WebResponse) {});
        }

        if (now - lastSendAt >= 0.10 && !g_stateSendPending) {
            lastSendAt = now;
            g_stateSendPending = true;
            auto p1 = this->m_player1;
            auto p2 = this->m_player2;
            auto pos1 = p1->getPosition();
            auto pos2 = p2 ? p2->getPosition() : pos1;
            matjson::Value state = matjson::Value::object();
            state["levelID"] = static_cast<int>(this->m_level->m_levelID);
            state["levelName"] = std::string(this->m_level->m_levelName.c_str());
            state["x"] = pos1.x;
            state["y"] = pos1.y;
            state["rotation"] = p1->getRotation();
            state["x2"] = pos2.x;
            state["y2"] = pos2.y;
            state["rotation2"] = p2 ? p2->getRotation() : 0.f;
            state["dual"] = p2 != nullptr;
            state["percent"] = this->getCurrentPercent();
            state["icon"] = GameManager::sharedState()->getPlayerFrame();
            state["color1"] = GameManager::sharedState()->getPlayerColor();
            state["color2"] = GameManager::sharedState()->getPlayerColor2();
            state["glow"] = GameManager::sharedState()->m_playerGlow;
            state["updatedAt"] = now;
            async::spawn(geode::utils::web::WebRequest()
                .timeout(std::chrono::seconds(5))
                .bodyJSON(state)
                .put(roomUrl(code, "/members/" + peer + "/state")),
                [](geode::utils::web::WebResponse) { g_stateSendPending = false; });
        }

        if (now - lastPollAt >= 0.25 && !g_statePollPending) {
            lastPollAt = now;
            g_statePollPending = true;
            Ref<GDRTCPlayLayer> self = this;
            async::spawn(geode::utils::web::WebRequest()
                .timeout(std::chrono::seconds(5))
                .get(roomUrl(code, "/members")),
                [self, code, peer](geode::utils::web::WebResponse response) {
                    g_statePollPending = false;
                    if (!response.ok() || g_activeRoomCode != code || PlayLayer::get() != self) return;
                    auto parsed = response.json();
                    if (!parsed || parsed.unwrap().isNull()) return;
                    auto members = parsed.unwrap();
                    std::map<std::string, bool> seen;
                    auto local = self->m_level;
                    if (!local) return;
                    for (auto const& [id, member] : members) {
                        if (id == peer) continue;
                        auto state = member["state"];
                        auto updatedValue = state["updatedAt"].asDouble();
                        auto levelIdValue = state["levelID"].asInt();
                        auto levelNameValue = state["levelName"].asString();
                        auto xValue = state["x"].asDouble();
                        auto yValue = state["y"].asDouble();
                        auto rotationValue = state["rotation"].asDouble();
                        auto iconValue = state["icon"].asInt();
                        auto color1Value = state["color1"].asInt();
                        auto color2Value = state["color2"].asInt();
                        auto glowValue = state["glow"].asBool();
                        auto percentValue = state["percent"].asDouble();
                        if (!updatedValue || !levelIdValue || !levelNameValue || !xValue || !yValue || !rotationValue) continue;
                        if (nowSeconds() - updatedValue.unwrap() > 3.0) continue;
                        if (levelIdValue.unwrap() != local->m_levelID ||
                            levelNameValue.unwrap() != std::string(local->m_levelName.c_str())) continue;

                        auto found = g_remoteAvatars.find(id);
                        CCNode* avatar = found == g_remoteAvatars.end() ? nullptr : found->second;
                        if (!avatar) {
                            avatar = SimplePlayer::create(iconValue ? iconValue.unwrap() : 1);
                            if (!avatar) avatar = CCSprite::createWithSpriteFrameName("player_01_001.png");
                            if (!avatar) avatar = CCLayerColor::create({90, 210, 255, 255}, 18.f, 18.f);
                            avatar->setScale(0.68f);
                            avatar->setID(("gd-rtc-remote-" + id).c_str());
                            auto label = CCLabelBMFont::create("", "goldFont.fnt");
                            label->setID("gd-rtc-remote-name");
                            label->setScale(0.38f);
                            label->setPosition({0.f, 22.f});
                            avatar->addChild(label, 2);
                            auto secondary = CCLayerColor::create({255, 210, 90, 255}, 10.f, 10.f);
                            secondary->setID("gd-rtc-remote-secondary");
                            secondary->setVisible(false);
                            avatar->addChild(secondary, 1);
                            self->addChild(avatar, 1000);
                            g_remoteAvatars[id] = avatar;
                        }
                        avatar->setVisible(true);
                        avatar->setPosition({static_cast<float>(xValue.unwrap()), static_cast<float>(yValue.unwrap())});
                        avatar->setRotation(static_cast<float>(rotationValue.unwrap()));
                        if (auto label = typeinfo_cast<CCLabelBMFont*>(avatar->getChildByID("gd-rtc-remote-name"))) {
                            auto nameValue = member["name"].asString();
                            std::string name = nameValue ? nameValue.unwrap() : "Player";
                            if (name.size() > 16) name.resize(16);
                            if (percentValue) name += " " + std::to_string(static_cast<int>(percentValue.unwrap())) + "%";
                            label->setString(name.c_str());
                        }
                        if (auto simple = typeinfo_cast<SimplePlayer*>(avatar)) {
                            auto gm = GameManager::sharedState();
                            if (color1Value) simple->setColor(gm->colorForIdx(static_cast<int>(color1Value.unwrap())));
                            if (color2Value) simple->setSecondColor(gm->colorForIdx(static_cast<int>(color2Value.unwrap())));
                            if (glowValue && glowValue.unwrap() && color2Value) {
                                simple->setGlowOutline(gm->colorForIdx(static_cast<int>(color2Value.unwrap())));
                            }
                        }
                        auto dualValue = state["dual"].asBool();
                        auto x2Value = state["x2"].asDouble();
                        auto y2Value = state["y2"].asDouble();
                        auto secondary = avatar->getChildByID("gd-rtc-remote-secondary");
                        bool dual = dualValue && dualValue.unwrap() && x2Value && y2Value;
                        if (secondary) {
                            secondary->setVisible(dual);
                            if (dual) secondary->setPosition({
                                static_cast<float>(x2Value.unwrap() - xValue.unwrap()),
                                static_cast<float>(y2Value.unwrap() - yValue.unwrap())
                            });
                        }
                        seen[id] = true;
                    }
                    for (auto it = g_remoteAvatars.begin(); it != g_remoteAvatars.end();) {
                        if (!seen[it->first]) {
                            if (it->second) it->second->removeFromParent();
                            it = g_remoteAvatars.erase(it);
                        } else ++it;
                    }
                });
        }
    }

    void onExit() {
        PlayLayer::onExit();
        g_remoteAvatars.clear();
        g_stateSendPending = false;
        g_statePollPending = false;
    }
};

class $modify(GDRTCPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        auto win = CCDirector::sharedDirector()->getWinSize();
        auto menu = CCMenu::create();
        menu->setID("gd-rtc-pause-menu");
        menu->setPosition({win.width - 74.f, 100.f});
        menu->setScale(std::min(1.0f, std::max(0.65f, win.width / 800.f)));
        this->addChild(menu, 1000);

        auto share = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Share Level", 108, true, "bigFont.fnt", "GJ_button_01.png", 30.f, .58f),
            this, menu_selector(GDRTCPauseLayer::onShareCurrentLevel));
        share->setPosition({0.f, 18.f});
        menu->addChild(share);

        auto open = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Open Shared", 108, true, "bigFont.fnt", "GJ_button_02.png", 30.f, .58f),
            this, menu_selector(GDRTCPauseLayer::onOpenSharedLevel));
        open->setPosition({0.f, -20.f});
        menu->addChild(open);
        auto saved = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Saved Levels", 108, true, "bigFont.fnt", "GJ_button_04.png", 30.f, .58f),
            this, menu_selector(GDRTCPauseLayer::onOpenSavedLevels));
        saved->setPosition({0.f, -58.f});
        menu->addChild(saved);
    }

    void onShareCurrentLevel(CCObject*) {
        auto play = PlayLayer::get();
        if (g_activeRoomCode.empty() || !play || !play->m_level) {
            FLAlertLayer::create("GD-RTC", "Join a room and start a level before sharing it.", "OK")->show();
            return;
        }
        std::string name = play->m_level->m_levelName.c_str();
        std::string data = play->m_level->m_levelString.c_str();
        if (data.empty()) {
            FLAlertLayer::create("GD-RTC", "This level's data is not available to share.", "OK")->show();
            return;
        }
        auto code = g_activeRoomCode;
        matjson::Value shared = matjson::Value::object();
        shared["name"] = name;
        shared["levelData"] = data;
        shared["levelID"] = static_cast<int>(play->m_level->m_levelID);
        shared["updatedAt"] = nowSeconds();
        shared["sharedBy"] = playerName();
        async::spawn(geode::utils::web::WebRequest()
            .timeout(std::chrono::seconds(15))
            .bodyJSON(shared)
            .put(roomUrl(code, "/sharedLevel")),
            [name, data](geode::utils::web::WebResponse response) {
                if (!response.ok()) {
                    FLAlertLayer::create("GD-RTC", "Couldn't share this level right now. Please try again.", "OK")->show();
                    return;
                }
                cacheSharedLevel(name, data);
                Notification::create("Level shared with room", NotificationIcon::Success)->show();
            });
    }

    void onOpenSavedLevels(CCObject*) {
        g_savedLevelsCache.clear();
        auto count = Mod::get()->getSavedValue<int>("saved-level-count", 0);
        if (count < 0) count = 0;
        if (count > 8) count = 8;
        for (int i = 0; i < count; ++i) {
            auto name = Mod::get()->getSavedValue<std::string>("saved-level-name-" + std::to_string(i), "");
            auto data = Mod::get()->getSavedValue<std::string>("saved-level-data-" + std::to_string(i), "");
            if (!data.empty()) g_savedLevelsCache.emplace_back(name.empty() ? "Shared level" : name, data);
        }
        if (g_savedLevelsCache.empty()) {
            FLAlertLayer::create("GD-RTC", "Shared levels you open will be saved here for later.", "OK")->show();
            return;
        }
        auto win = CCDirector::sharedDirector()->getWinSize();
        auto scene = CCScene::create();
        auto bg = CCLayerColor::create({12, 20, 40, 255});
        bg->setContentSize(win);
        scene->addChild(bg);
        auto title = CCLabelBMFont::create("Saved Shared Levels", "bigFont.fnt");
        title->setPosition({win.width / 2, win.height - 42.f});
        title->setScale(0.72f);
        scene->addChild(title);
        auto menu = CCMenu::create();
        menu->setPosition({win.width / 2, win.height / 2 + 8.f});
        scene->addChild(menu);
        auto shown = std::min<size_t>(5, g_savedLevelsCache.size());
        for (size_t i = 0; i < shown; ++i) {
            auto index = g_savedLevelsCache.size() - 1 - i;
            auto label = g_savedLevelsCache[index].first;
            if (label.size() > 24) label.resize(24);
            auto button = CCMenuItemSpriteExtra::create(
                ButtonSprite::create(label.c_str(), 250, true, "bigFont.fnt", "GJ_button_01.png", 28.f, .52f),
                this, menu_selector(GDRTCPauseLayer::onOpenSavedLevelByIndex));
            button->setTag(static_cast<int>(index));
            button->setPosition({0.f, 74.f - static_cast<float>(i) * 42.f});
            menu->addChild(button);
        }
        auto backMenu = CCMenu::create();
        backMenu->setPosition({win.width / 2, 32.f});
        scene->addChild(backMenu);
        auto back = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Back", 90, true, "bigFont.fnt", "GJ_button_04.png", 28.f, .6f),
            this, menu_selector(GDRTCPauseLayer::onCloseSavedLevels));
        backMenu->addChild(back);
        CCDirector::sharedDirector()->pushScene(scene);
    }

    void onOpenSavedLevelByIndex(CCObject* sender) {
        auto item = typeinfo_cast<CCMenuItemSpriteExtra*>(sender);
        if (!item) return;
        auto index = item->getTag();
        if (index < 0 || static_cast<size_t>(index) >= g_savedLevelsCache.size()) return;
        auto const level = g_savedLevelsCache[static_cast<size_t>(index)];
        openLevelEditor(level.first, level.second);
    }

    void onCloseSavedLevels(CCObject*) {
        CCDirector::sharedDirector()->popScene();
    }

    void onOpenSharedLevel(CCObject*) {
        auto code = g_activeRoomCode;
        if (code.empty()) {
            auto name = Mod::get()->getSavedValue<std::string>("saved-level-name", "");
            auto data = Mod::get()->getSavedValue<std::string>("saved-level-data", "");
            if (data.empty()) {
                FLAlertLayer::create("GD-RTC", "Join a room to receive a shared level first.", "OK")->show();
                return;
            }
            openLevelEditor(name.empty() ? "Shared level" : name, data);
            return;
        }
        async::spawn(geode::utils::web::WebRequest()
            .timeout(std::chrono::seconds(12))
            .get(roomUrl(code, "/sharedLevel")),
            [](geode::utils::web::WebResponse response) {
                if (!response.ok()) {
                    FLAlertLayer::create("GD-RTC", "Couldn't load the shared level right now.", "OK")->show();
                    return;
                }
                auto parsed = response.json();
                if (!parsed || parsed.unwrap().isNull()) {
                    FLAlertLayer::create("GD-RTC", "No level has been shared to this room yet.", "OK")->show();
                    return;
                }
                auto nameValue = parsed.unwrap()["name"].asString();
                auto dataValue = parsed.unwrap()["levelData"].asString();
                if (!dataValue || dataValue.unwrap().empty()) {
                    FLAlertLayer::create("GD-RTC", "The shared level data is incomplete.", "OK")->show();
                    return;
                }
                std::string name = nameValue ? nameValue.unwrap() : "Shared level";
                std::string data = dataValue.unwrap();
                cacheSharedLevel(name, data);
                openLevelEditor(name, data);
            });
    }
};
