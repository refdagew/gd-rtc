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
#include <rtc/rtc.hpp>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <deque>
#include <random>
#include <set>
#include <map>
#include <mutex>
#include <string>
#include <sstream>
#include <vector>

using namespace geode::prelude;

namespace {
    constexpr char kDatabaseUrl[] = "https://gd-rtc-default-rtdb.firebaseio.com";
    constexpr char kAlphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    std::string g_activeRoomCode;
    std::string g_activePeerId;
    bool g_refreshPending = false;
    bool g_roomOperationPending = false;
    std::map<std::string, std::string> g_peerNames;
    std::map<std::string, CCNode*> g_remoteAvatars;
    std::map<std::string, CCNode*> g_remoteSecondaryAvatars;
    std::map<std::string, CCLabelBMFont*> g_remoteNameLabels;
    std::map<std::string, SimplePlayer*> g_remoteProgressIcons;
    std::map<std::string, std::pair<CCPoint, float>> g_remoteAvatarTargets;
    std::map<std::string, std::pair<CCPoint, float>> g_remoteSecondaryTargets;
    std::map<std::string, double> g_remoteAvatarLastSeen;
    void clearRemoteAvatars() {
        for (auto const& [id, node] : g_remoteAvatars) { (void)id; if (node) node->removeFromParent(); }
        for (auto const& [id, node] : g_remoteSecondaryAvatars) { (void)id; if (node) node->removeFromParent(); }
        for (auto const& [id, node] : g_remoteNameLabels) { (void)id; if (node) node->removeFromParent(); }
        for (auto const& [id, node] : g_remoteProgressIcons) { (void)id; if (node) node->removeFromParent(); }
        g_remoteAvatars.clear();
        g_remoteSecondaryAvatars.clear();
        g_remoteNameLabels.clear();
        g_remoteProgressIcons.clear();
        g_remoteAvatarTargets.clear();
        g_remoteSecondaryTargets.clear();
        g_remoteAvatarLastSeen.clear();
    }
    struct GDRTCPeerSession {
        std::shared_ptr<rtc::PeerConnection> pc;
        std::shared_ptr<rtc::DataChannel> channel;
        std::mutex channelMutex;
        bool offerer = false;
        std::atomic_bool remoteDescriptionSet{false};
        std::atomic_bool signalPollPending{false};
        std::chrono::steady_clock::time_point createdAt = std::chrono::steady_clock::now();
        std::chrono::steady_clock::time_point lastSignalPollAt{};
        std::set<std::string> seenCandidates;
        std::mutex candidateMutex;
    };
    std::map<std::string, std::shared_ptr<GDRTCPeerSession>> g_rtcPeers;
    std::mutex g_rtcMessagesMutex;
    std::deque<std::pair<std::string, std::string>> g_rtcMessages;

    std::string roomUrl(std::string const& code, std::string const& suffix);

    std::shared_ptr<rtc::DataChannel> rtcGetChannel(std::shared_ptr<GDRTCPeerSession> const& session) {
        std::lock_guard<std::mutex> lock(session->channelMutex);
        return session->channel;
    }

    void rtcCloseAllPeers() {
        for (auto const& [remoteId, session] : g_rtcPeers) {
            (void)remoteId;
            try {
                auto channel = rtcGetChannel(session);
                if (channel) channel->close();
                if (session->pc) session->pc->close();
            } catch (std::exception const& e) {
                log::debug("GD-RTC peer close: {}", e.what());
            }
        }
        g_rtcPeers.clear();
        std::lock_guard<std::mutex> lock(g_rtcMessagesMutex);
        g_rtcMessages.clear();
    }

    void rtcEnsurePeer(std::string const& remoteId) {
        if (g_activeRoomCode.empty() || g_activePeerId.empty() || remoteId.empty() ||
            remoteId == g_activePeerId || g_rtcPeers.count(remoteId)) return;
        auto room = g_activeRoomCode;
        auto self = g_activePeerId;
        auto session = std::make_shared<GDRTCPeerSession>();
        session->offerer = self < remoteId;
        rtc::Configuration config; // Deliberately no STUN or TURN servers.
        try {
            session->pc = std::make_shared<rtc::PeerConnection>(config);
            session->pc->onLocalDescription([room, self, remoteId, session](rtc::Description description) {
                if (g_activeRoomCode != room || g_activePeerId != self) return;
                matjson::Value body = matjson::Value::object();
                body["type"] = std::string(description.typeString());
                body["sdp"] = std::string(description);
                auto type = std::string(description.typeString());
                async::spawn(geode::utils::web::WebRequest()
                    .timeout(std::chrono::seconds(8))
                    .bodyJSON(body)
                    .put(roomUrl(room, "/members/" + self + "/signals/" + remoteId + "/" + type)),
                    [](geode::utils::web::WebResponse) {});
            });
            session->pc->onLocalCandidate([room, self, remoteId](rtc::Candidate candidate) {
                if (g_activeRoomCode != room || g_activePeerId != self) return;
                matjson::Value body = matjson::Value::object();
                body["candidate"] = std::string(candidate.candidate());
                body["mid"] = std::string(candidate.mid());
                async::spawn(geode::utils::web::WebRequest()
                    .timeout(std::chrono::seconds(8))
                    .bodyJSON(body)
                    .post(roomUrl(room, "/members/" + self + "/signals/" + remoteId + "/candidates")),
                    [](geode::utils::web::WebResponse) {});
            });
            session->pc->onDataChannel([remoteId, session](std::shared_ptr<rtc::DataChannel> channel) {
                {
                    std::lock_guard<std::mutex> lock(session->channelMutex);
                    session->channel = channel;
                }
                channel->onMessage([remoteId](rtc::message_variant message) {
                    if (!std::holds_alternative<rtc::string>(message)) return;
                    std::lock_guard<std::mutex> lock(g_rtcMessagesMutex);
                    if (g_rtcMessages.size() > 128) g_rtcMessages.pop_front();
                    g_rtcMessages.emplace_back(remoteId, std::get<rtc::string>(std::move(message)));
                });
            });
            session->pc->onStateChange([remoteId](rtc::PeerConnection::State state) {
                if (state == rtc::PeerConnection::State::Failed ||
                    state == rtc::PeerConnection::State::Closed) {
                    log::warn("GD-RTC WebRTC peer {} disconnected", remoteId);
                }
            });
            g_rtcPeers[remoteId] = session;
            if (session->offerer) {
                rtc::DataChannelInit channelConfig;
                channelConfig.reliability.unordered = true;
                channelConfig.reliability.maxPacketLifeTime = std::chrono::milliseconds(100);
                auto channel = session->pc->createDataChannel("gd-rtc-gameplay", channelConfig);
                {
                    std::lock_guard<std::mutex> lock(session->channelMutex);
                    session->channel = channel;
                }
                channel->onMessage([remoteId](rtc::message_variant message) {
                    if (!std::holds_alternative<rtc::string>(message)) return;
                    std::lock_guard<std::mutex> lock(g_rtcMessagesMutex);
                    if (g_rtcMessages.size() > 128) g_rtcMessages.pop_front();
                    g_rtcMessages.emplace_back(remoteId, std::get<rtc::string>(std::move(message)));
                });
                session->pc->setLocalDescription(rtc::Description::Type::Offer);
            }
        } catch (std::exception const& e) {
            log::error("GD-RTC WebRTC setup failed: {}", e.what());
            g_rtcPeers.erase(remoteId);
        }
    }

    void rtcPollSignals() {
        if (g_activeRoomCode.empty() || g_activePeerId.empty()) return;
        auto room = g_activeRoomCode;
        auto self = g_activePeerId;
        std::vector<std::string> timedOutPeers;
        for (auto const& [remoteId, session] : g_rtcPeers) {
            // Stop touching RTDB entirely once the peer-to-peer channel is live.
            auto channel = rtcGetChannel(session);
            if (channel && channel->isOpen() &&
                session->pc->state() == rtc::PeerConnection::State::Connected) continue;

            // Avoid hammering Firebase if direct ICE connectivity is impossible.
            // Poll quickly during initial negotiation, then back off and give up
            // after 90 seconds; no relay server is configured by design.
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - session->createdAt).count();
            if (elapsed >= 90) {
                timedOutPeers.push_back(remoteId);
                continue;
            }
            auto interval = elapsed < 15 ? 1 : (elapsed < 45 ? 3 : 8);
            if (session->lastSignalPollAt.time_since_epoch().count() != 0 &&
                std::chrono::duration_cast<std::chrono::seconds>(now - session->lastSignalPollAt).count() < interval) continue;
            session->lastSignalPollAt = now;
            if (session->signalPollPending.exchange(true)) continue;
            async::spawn(geode::utils::web::WebRequest()
                .timeout(std::chrono::seconds(6))
                .get(roomUrl(room, "/members/" + remoteId + "/signals/" + self)),
                [room, self, remoteId, session](geode::utils::web::WebResponse response) {
                    session->signalPollPending = false;
                    if (g_activeRoomCode != room || g_activePeerId != self || !response.ok()) return;
                    auto parsed = response.json();
                    if (!parsed || parsed.unwrap().isNull()) return;
                    auto signal = parsed.unwrap();
                    auto offer = signal["offer"];
                    auto answer = signal["answer"];
                    try {
                        if (!session->remoteDescriptionSet && session->offerer == false) {
                            auto sdp = offer["sdp"].asString();
                            auto type = offer["type"].asString();
                            if (sdp && type && !sdp.unwrap().empty()) {
                                session->pc->setRemoteDescription(rtc::Description(sdp.unwrap(), type.unwrap()));
                                session->remoteDescriptionSet = true;
                                session->pc->setLocalDescription(rtc::Description::Type::Answer);
                            }
                        } else if (!session->remoteDescriptionSet && session->offerer) {
                            auto sdp = answer["sdp"].asString();
                            auto type = answer["type"].asString();
                            if (sdp && type && !sdp.unwrap().empty()) {
                                session->pc->setRemoteDescription(rtc::Description(sdp.unwrap(), type.unwrap()));
                                session->remoteDescriptionSet = true;
                            }
                        }
                        if (session->remoteDescriptionSet) {
                            auto candidates = signal["candidates"];
                            for (auto const& [candidateId, value] : candidates) {
                                {
                                    std::lock_guard<std::mutex> lock(session->candidateMutex);
                                    if (session->seenCandidates.count(candidateId)) continue;
                                    session->seenCandidates.insert(candidateId);
                                }
                                auto candidate = value["candidate"].asString();
                                auto mid = value["mid"].asString();
                                if (candidate && mid) session->pc->addRemoteCandidate(
                                    rtc::Candidate(candidate.unwrap(), mid.unwrap()));
                            }
                        }
                    } catch (std::exception const& e) {
                        log::warn("GD-RTC WebRTC signaling error: {}", e.what());
                    }
                });
        }
        for (auto const& remoteId : timedOutPeers) {
            auto it = g_rtcPeers.find(remoteId);
            if (it == g_rtcPeers.end()) continue;
            try {
                auto channel = rtcGetChannel(it->second);
                if (channel) channel->close();
                if (it->second->pc) it->second->pc->close();
            } catch (...) {}
            g_rtcPeers.erase(it);
            log::warn("GD-RTC: direct connection to {} timed out; will retry discovery", remoteId);
        }
    }


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
        std::uniform_int_distribution<int> pick(0, 9);
        std::string code;
        for (int i = 0; i < 5; ++i) code += static_cast<char>('0' + pick(rng));
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
        auto bodyResult = response.string();
        std::string body = bodyResult ? bodyResult.unwrap() : "";
        log::warn("GD-RTC room service request failed: HTTP {}, error='{}', body='{}'",
            response.code(), response.errorMessage(), body.substr(0, 240));
        // Keep implementation details out of the player-facing UI. The full
        // response is available in Geode's log if a report is needed.
        return "Couldn't connect to the room service. Check your connection and try again.";
    }

    void addGeodePanelBackground(CCNode* panel) {
        auto bg = CCSprite::createWithSpriteFrameName("GJ_square01.png");
        if (bg && bg->getContentSize().width > 0.f && bg->getContentSize().height > 0.f) {
            bg->setScaleX(360.f / bg->getContentSize().width);
            bg->setScaleY(420.f / bg->getContentSize().height);
            bg->setPosition({0.f, 0.f});
            bg->setID("gd-rtc-panel-background");
            panel->addChild(bg, -1);
        } else {
            auto fallback = CCLayerColor::create({105, 69, 43, 248});
            fallback->setContentSize({360.f, 420.f});
            fallback->setPosition({-180.f, -210.f});
            panel->addChild(fallback, -1);
        }
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
            if (auto item = menu->getChildByID("gd-rtc-refresh")) item->setVisible(false);
            if (auto item = menu->getChildByID("gd-rtc-leave")) item->setVisible(inRoom);
        }
        if (auto input = typeinfo_cast<geode::TextInput*>(panel->getChildByID("gd-rtc-room-code-input"))) {
            input->setEnabled(!inRoom);
            input->setVisible(!inRoom);
        }
        if (auto codeLabel = typeinfo_cast<CCLabelBMFont*>(panel->getChildByID("gd-rtc-room-code-label"))) {
            codeLabel->setVisible(inRoom);
            if (inRoom) codeLabel->setString(("Room " + g_activeRoomCode).c_str());
        }
        if (menu) {
            if (auto item = menu->getChildByID("gd-rtc-create")) item->setPosition({0.f, 0.f});
            if (auto item = menu->getChildByID("gd-rtc-join")) item->setPosition({-76.f, -40.f});
            if (auto item = menu->getChildByID("gd-rtc-refresh")) item->setVisible(false);
            if (auto item = menu->getChildByID("gd-rtc-leave")) item->setPosition({0.f, -164.f});
            if (auto item = menu->getChildByID("gd-rtc-close")) item->setVisible(true);
        }
        if (auto subtitle = typeinfo_cast<CCLabelBMFont*>(panel->getChildByID("gd-rtc-subtitle"))) {
            subtitle->setString(inRoom ? "Invite friends with this room code" : "Create a room or join with a 5-digit code");
        }
        for (auto id : {"gd-rtc-members-back", "gd-rtc-room-members", "gd-rtc-self-name", "gd-rtc-self-state", "gd-rtc-member-menu"}) {
            if (auto node = panel->getChildByID(id)) node->setVisible(inRoom);
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

    void keyBackClicked() {
        if (this->getChildByID("gd-rtc-room-panel")) {
            onCloseRooms(nullptr);
            return;
        }
        MenuLayer::keyBackClicked();
    }

    void onGDRTC(CCObject*) {
        if (auto existing = this->getChildByID("gd-rtc-room-panel")) {
            existing->removeFromParent();
            if (auto shade = this->getChildByID("gd-rtc-room-shade")) shade->removeFromParent();
            setBackgroundMenusEnabled(true);
            return;
        }

        setBackgroundMenusEnabled(false);
        this->setKeypadEnabled(true);
        auto win = CCDirector::sharedDirector()->getWinSize();
        // Keep the dimmer in screen coordinates. Putting it inside the scaled
        // panel used to shrink the "full-screen" shade on smaller displays.
        auto shade = CCLayerColor::create({0, 0, 0, 185});
        shade->setID("gd-rtc-room-shade");
        shade->setContentSize(win);
        shade->setPosition({0.f, 0.f});
        this->addChild(shade, 999);
        auto panel = CCNode::create();
        panel->setID("gd-rtc-room-panel");
        panel->setZOrder(1000);
        panel->setPosition({win.width / 2, win.height / 2});
        auto uiScale = std::min(1.0f, std::min((win.width - 24.f) / 360.f, (win.height - 24.f) / 390.f));
        panel->setScale(std::max(0.55f, uiScale));
        this->addChild(panel);

        // Use Geometry Dash's own brown panel asset rather than a custom navy card.
        addGeodePanelBackground(panel);

        auto title = CCLabelBMFont::create("GD-RTC", "bigFont.fnt");
        title->setScale(0.86f);
        title->setPosition({0, 171.f});
        panel->addChild(title);

        auto subtitle = CCLabelBMFont::create("Create a room or enter a friend's code", "goldFont.fnt");
        subtitle->setID("gd-rtc-subtitle");
        subtitle->setScale(0.42f);
        subtitle->setPosition({0, 143.f});
        panel->addChild(subtitle);

        auto input = geode::TextInput::create(100.f, "00000");
        input->setID("gd-rtc-room-code-input");
        input->setCommonFilter(geode::CommonFilter::Uint);
        input->setMaxCharCount(5);
        input->setTextAlign(geode::TextInputAlign::Center);
        input->setPosition({48.f, -15.f});
        panel->addChild(input);

        auto codeLabel = CCLabelBMFont::create("", "bigFont.fnt");
        codeLabel->setID("gd-rtc-room-code-label");
        codeLabel->setScale(0.72f);
        codeLabel->setPosition({0.f, 111.f});
        codeLabel->setVisible(false);
        panel->addChild(codeLabel);

        auto membersBack = CCLayerColor::create({69, 44, 29, 220});
        membersBack->setID("gd-rtc-members-back");
        membersBack->setContentSize({320.f, 218.f});
        membersBack->setPosition({-160.f, -151.f});
        panel->addChild(membersBack, 0);

        auto members = CCLabelBMFont::create("Players in room:", "goldFont.fnt");
        members->setID("gd-rtc-room-members");
        members->setScale(0.48f);
        members->setPosition({-126.f, 72.f});
        panel->addChild(members);
        auto selfName = CCLabelBMFont::create("", "goldFont.fnt");
        selfName->setID("gd-rtc-self-name");
        selfName->setAnchorPoint({0.f, 0.5f});
        selfName->setScale(0.46f);
        selfName->setPosition({-126.f, 48.f});
        panel->addChild(selfName);
        auto selfState = CCLabelBMFont::create("", "goldFont.fnt");
        selfState->setID("gd-rtc-self-state");
        selfState->setAnchorPoint({0.f, 0.5f});
        selfState->setScale(0.34f);
        selfState->setPosition({-116.f, 34.f});
        panel->addChild(selfState);

        auto memberMenu = CCMenu::create();
        memberMenu->setID("gd-rtc-member-menu");
        memberMenu->setPosition({0, 0});
        panel->addChild(memberMenu);

        auto status = CCLabelBMFont::create("Ready", "goldFont.fnt");
        status->setID("gd-rtc-room-status");
        status->setScale(0.38f);
        status->setPosition({0, -194.f});
        panel->addChild(status);

        auto menu = CCMenu::create();
        menu->setID("gd-rtc-room-panel-menu");
        menu->setPosition({0, 25.f});
        panel->addChild(menu);

        auto createBtn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Create Room", 140, true, "bigFont.fnt", "GJ_button_01.png", 30.f, .65f),
            this, menu_selector(GDRTCMenuLayer::onCreateRoom));
        createBtn->setID("gd-rtc-create");
        createBtn->setPosition({0.f, 0.f});
        menu->addChild(createBtn);

        auto joinBtn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Join", 72, true, "bigFont.fnt", "GJ_button_01.png", 30.f, .65f),
            this, menu_selector(GDRTCMenuLayer::onJoinRoom));
        joinBtn->setID("gd-rtc-join");
        joinBtn->setPosition({-76.f, -40.f});
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

        auto closeSprite = CCSprite::createWithSpriteFrameName("GJ_closeBtn_001.png");
        if (!closeSprite) closeSprite = CCSprite::createWithSpriteFrameName("GJ_deleteBtn_001.png");
        auto closeBtn = CCMenuItemSpriteExtra::create(closeSprite, this, menu_selector(GDRTCMenuLayer::onCloseRooms));
        closeBtn->setID("gd-rtc-close");
        closeBtn->setPosition({-160.f, 190.f});
        closeBtn->setScale(0.72f);
        auto closeMenu = CCMenu::create(closeBtn, nullptr);
        closeMenu->setID("gd-rtc-close-menu");
        closeMenu->setPosition({0, 0});
        panel->addChild(closeMenu, 20);

        updateRoomControls(!g_activeRoomCode.empty());
        this->schedule(schedule_selector(GDRTCMenuLayer::pollRoom), 15.0f);
        this->schedule(schedule_selector(GDRTCMenuLayer::pollSignaling), 1.0f);
        if (!g_activeRoomCode.empty()) {
            input->setString(g_activeRoomCode);
            setRoomStatus("Room " + g_activeRoomCode);
            onRefreshMembers(nullptr);
        }
    }

    void onCloseRooms(CCObject*) {
        this->unschedule(schedule_selector(GDRTCMenuLayer::pollRoom));
        this->unschedule(schedule_selector(GDRTCMenuLayer::pollSignaling));
        if (auto panel = this->getChildByID("gd-rtc-room-panel")) panel->removeFromParent();
        if (auto shade = this->getChildByID("gd-rtc-room-shade")) shade->removeFromParent();
        setBackgroundMenusEnabled(true);
    }

    void onCreateRoom(CCObject*) {
        if (!this->getChildByID("gd-rtc-room-panel") || g_roomOperationPending) return;
        g_roomOperationPending = true;
        setRoomStatus("Creating room...");
        // Avoid downloading the entire database just to garbage-collect rooms:
        // each client only reads its own room and its small member directory.
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
        if (code.size() != 5 || !std::all_of(code.begin(), code.end(), [](unsigned char ch) { return std::isdigit(ch); })) {
            setRoomStatus("Enter the 5-digit room code.");
            return;
        }

        g_roomOperationPending = true;
        setRoomStatus("Joining room...");
        auto peer = makePeerId();
        auto now = nowSeconds();
        Ref<GDRTCMenuLayer> self = this;
        async::spawn(geode::utils::web::WebRequest()
            .timeout(std::chrono::seconds(12))
            .get(roomUrl(code, "/members")),
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

                auto members = parsed.unwrap();
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

    void pollSignaling(float) {
        // Signaling requests stop automatically for connected peers.
        // Gameplay packets never pass through Firebase.
        if (!g_activeRoomCode.empty()) rtcPollSignals();
    }

    void pollRoom(float) {
        if (!this->getChildByID("gd-rtc-room-panel")) {
            this->unschedule(schedule_selector(GDRTCMenuLayer::pollRoom));
            this->unschedule(schedule_selector(GDRTCMenuLayer::pollSignaling));
            return;
        }
        if (!g_activeRoomCode.empty()) {
            onRefreshMembers(nullptr);
            auto code = g_activeRoomCode;
            auto peer = g_activePeerId;
            static double lastHeartbeatAt = 0.0;
            auto now = nowSeconds();
            if (now - lastHeartbeatAt >= 30.0) {
                lastHeartbeatAt = now;
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
    }

    void onRefreshMembers(CCObject*) {
        if (g_refreshPending) return;
        if (g_activeRoomCode.empty()) {
            setRoomMembers("Your room members will appear here.");
            return;
        }
        g_refreshPending = true;
        setRoomStatus("Refreshing players...");
        auto code = g_activeRoomCode;
        Ref<GDRTCMenuLayer> self = this;
        async::spawn(geode::utils::web::WebRequest()
            .timeout(std::chrono::seconds(10))
            .get(roomUrl(code, "/members")),
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
                auto members = parsed.unwrap();
                std::string names = "Players";
                unsigned int count = 0;
                g_roomMemberIds.clear();
                if (auto panel = self->getChildByID("gd-rtc-room-panel")) {
                    if (auto menu = typeinfo_cast<CCMenu*>(panel->getChildByID("gd-rtc-member-menu"))) menu->removeAllChildren();
                    if (auto own = typeinfo_cast<CCLabelBMFont*>(panel->getChildByID("gd-rtc-self-name"))) own->setString((playerName() + " (you)").c_str());
                    if (auto ownState = typeinfo_cast<CCLabelBMFont*>(panel->getChildByID("gd-rtc-self-state"))) {
                        auto play = PlayLayer::get();
                        if (play && play->m_level) {
                            auto levelName = std::string(play->m_level->m_levelName.c_str());
                            if (levelName.size() > 30) levelName.resize(30);
                            auto levelID = static_cast<int>(play->m_level->m_levelID);
                            auto levelTypeValue = static_cast<int>(play->m_level->m_levelType);
                            std::string typeName = levelTypeValue == 1 ? "main" : (levelTypeValue == 4 ? "online" : "custom");
                            ownState->setString(("PLAYING " + levelName + " (" + std::to_string(levelID) + ") [" + typeName + "]").c_str());
                        } else ownState->setString("");
                    }
                }
                for (auto const& [id, value] : members) {
                    if (id == g_activePeerId) continue;
                    auto nameValue = value["name"].asString();
                    g_peerNames[id] = nameValue ? nameValue.unwrap() : "Player";
                    rtcEnsurePeer(id);
                    if (count >= 7) break;
                    auto result = value["name"].asString();
                    std::string name = result ? result.unwrap() : "Player";
                    if (name.size() > 16) name.resize(16);
                    auto state = value["currentLevel"];
                    auto level = state["name"].asString();
                    auto levelID = state["levelID"].asInt();
                    auto levelType = state["levelType"].asString();
                    auto updated = state["updatedAt"].asDouble();
                    bool playing = level && levelID && updated && nowSeconds() - updated.unwrap() < 35.0;
                    std::string detail;
                    if (playing) {
                        std::string levelName = level.unwrap();
                        if (levelName.size() > 28) levelName.resize(28);
                        detail = "PLAYING " + levelName + " (" + std::to_string(levelID.unwrap()) + ")";
                        if (levelType) detail += " [" + levelType.unwrap() + "]";
                    }
                    auto rowIndex = count;
                    g_roomMemberIds.push_back(id);
                    if (auto panel = self->getChildByID("gd-rtc-room-panel")) {
                        if (auto menu = typeinfo_cast<CCMenu*>(panel->getChildByID("gd-rtc-member-menu"))) {
                            auto nameLabel = CCLabelBMFont::create(name.c_str(), "goldFont.fnt");
                            nameLabel->setAnchorPoint({0.f, 0.5f});
                            nameLabel->setScale(0.34f);
                            nameLabel->setPosition({-126.f, 24.f - static_cast<float>(rowIndex) * 22.f});
                            menu->addChild(nameLabel);
                            auto detailLabel = CCLabelBMFont::create(detail.c_str(), "goldFont.fnt");
                            detailLabel->setAnchorPoint({0.f, 0.5f});
                            detailLabel->setScale(0.25f);
                            detailLabel->setPosition({-116.f, 14.f - static_cast<float>(rowIndex) * 22.f});
                            menu->addChild(detailLabel);
                            auto joinButton = CCMenuItemSpriteExtra::create(
                                ButtonSprite::create("Join", 68, true, "bigFont.fnt", playing ? "GJ_button_01.png" : "GJ_button_04.png", 22.f, .58f),
                                self, menu_selector(GDRTCMenuLayer::onGoToPlayerLevel));
                            joinButton->setTag(static_cast<int>(g_roomMemberIds.size() - 1));
                            joinButton->setPosition({119.f, 20.f - static_cast<float>(rowIndex) * 22.f});
                            joinButton->setEnabled(playing);
                            menu->addChild(joinButton);
                        }
                    }
                    ++count;
                }
                if (auto panel = self->getChildByID("gd-rtc-room-panel")) {
                    if (auto label = typeinfo_cast<CCLabelBMFont*>(panel->getChildByID("gd-rtc-room-members"))) label->setString(("Players (" + std::to_string(count + 1) + "/8)").c_str());
                }
                if (g_activeRoomCode == code) self->setRoomStatus("Room " + code);
            });
    }

    void onGoToPlayerLevel(CCObject* sender) {
        auto item = typeinfo_cast<CCMenuItemSpriteExtra*>(sender);
        if (!item || g_activeRoomCode.empty()) return;
        auto index = item->getTag();
        if (index < 0 || static_cast<size_t>(index) >= g_roomMemberIds.size()) return;
        auto code = g_activeRoomCode;
        auto peer = g_roomMemberIds[static_cast<size_t>(index)];
        setRoomStatus("Joining level...");
        Ref<GDRTCMenuLayer> self = this;
        async::spawn(geode::utils::web::WebRequest()
            .timeout(std::chrono::seconds(12))
            .get(roomUrl(code, "/levels/" + peer)),
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
                auto idValue = parsed.unwrap()["levelID"].asInt();
                auto typeValue = parsed.unwrap()["levelType"].asString();
                std::string name = nameValue ? nameValue.unwrap() : "Shared level";
                std::string data = dataValue ? dataValue.unwrap() : "";
                GJGameLevel* level = nullptr;
                if (typeValue && typeValue.unwrap() == "main" && idValue) {
                    level = GameLevelManager::sharedState()->getMainLevel(static_cast<int>(idValue.unwrap()), false);
                }
                if (!level && !data.empty()) {
                    level = GJGameLevel::create();
                    level->m_levelName = name.c_str();
                    level->m_levelString = data.c_str();
                    level->m_levelID = idValue ? static_cast<int>(idValue.unwrap()) : 0;
                }
                if (!level) {
                    self->setRoomStatus("That level is not available yet.");
                    return;
                }
                auto scene = PlayLayer::scene(level, false, false);
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
                    std::vector<std::string> remoteIds;
                    for (auto const& [remoteId, session] : g_rtcPeers) {
                        (void)session;
                        remoteIds.push_back(remoteId);
                    }
                    rtcCloseAllPeers();
                    g_activeRoomCode.clear();
                    g_activePeerId.clear();
                    g_peerNames.clear();
                    g_roomMemberIds.clear();
                    clearRemoteAvatars();
                    // Remove this peer's incoming signal mailbox from every other
                    // participant. Deleting our member record removes our outgoing
                    // mailbox as part of the same room subtree.
                    for (auto const& remoteId : remoteIds) {
                        async::spawn(geode::utils::web::WebRequest()
                            .timeout(std::chrono::seconds(5))
                            .send("DELETE", roomUrl(code, "/members/" + remoteId + "/signals/" + peer)),
                            [](geode::utils::web::WebResponse) {});
                    }
                }
                self->updateRoomControls(false);
                self->setRoomMembers("Players in room (1/8)\nWaiting for friends to join");
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



// Gameplay state is intentionally NOT written to Firebase. Firebase is only for
// room membership, level sharing, and WebRTC offer/answer/ICE signaling.
// A single level snapshot per level is low-frequency metadata, not gameplay traffic.
class $modify(GDRTCPlayLayer, PlayLayer) {
    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);
        if (g_activeRoomCode.empty() || g_activePeerId.empty() || !this->m_player1 || !this->m_level) return;

        static double lastPublishedLevelAt = 0.0;
        static double lastCurrentLevelHeartbeatAt = 0.0;
        static std::string lastPublishedLevelKey;
        static double lastStateSendAt = 0.0;
        static double lastSignalPollAt = 0.0;
        static double lastPeerDiscoveryAt = 0.0;
        auto now = nowSeconds();
        auto code = g_activeRoomCode;
        auto peer = g_activePeerId;
        auto levelName = std::string(this->m_level->m_levelName.c_str());
        auto levelData = std::string(this->m_level->m_levelString.c_str());
        auto levelKey = code + "/" + peer + "/" + std::to_string(static_cast<int>(this->m_level->m_levelID)) + "/" + levelName;
        auto levelId = static_cast<int>(this->m_level->m_levelID);
        auto levelTypeValue = static_cast<int>(this->m_level->m_levelType);
        std::string levelType = levelTypeValue == 1 ? "main" : (levelTypeValue == 4 ? "online" : "custom");
        if (levelKey != lastPublishedLevelKey && levelData.size() <= 950000) {
            lastPublishedLevelKey = levelKey;
            lastPublishedLevelAt = now;
            matjson::Value levelInfo = matjson::Value::object();
            levelInfo["name"] = levelName;
            levelInfo["levelData"] = levelData;
            levelInfo["levelID"] = levelId;
            levelInfo["levelType"] = levelType;
            levelInfo["updatedAt"] = now;
            // Large level payloads live outside /members so normal room-list
            // reads never download megabytes of custom level data.
            async::spawn(geode::utils::web::WebRequest()
                .timeout(std::chrono::seconds(15))
                .bodyJSON(levelInfo)
                .put(roomUrl(code, "/levels/" + peer)),
                [](geode::utils::web::WebResponse) {});
            matjson::Value levelMeta = matjson::Value::object();
            levelMeta["name"] = levelName;
            levelMeta["levelID"] = levelId;
            levelMeta["levelType"] = levelType;
            levelMeta["updatedAt"] = now;
            async::spawn(geode::utils::web::WebRequest()
                .timeout(std::chrono::seconds(8))
                .bodyJSON(levelMeta)
                .put(roomUrl(code, "/members/" + peer + "/currentLevel")),
                [](geode::utils::web::WebResponse) {});
        }

        // Refresh only low-frequency "currently playing" metadata; gameplay packets stay peer-to-peer.
        if (now - lastCurrentLevelHeartbeatAt >= 10.0) {
            lastCurrentLevelHeartbeatAt = now;
            matjson::Value current = matjson::Value::object();
            current["name"] = levelName;
            current["levelID"] = levelId;
            current["levelType"] = levelType;
            current["updatedAt"] = now;
            async::spawn(geode::utils::web::WebRequest().timeout(std::chrono::seconds(6))
                .bodyJSON(current).put(roomUrl(code, "/members/" + peer + "/currentLevel")),
                [](geode::utils::web::WebResponse) {});
        }

        // Firebase is touched only for SDP/ICE negotiation and low-frequency level metadata.
        if (now - lastSignalPollAt >= 1.0) {
            lastSignalPollAt = now;
            rtcPollSignals();
        }

        // Discover new room members occasionally, never at gameplay frame rate.
        if (now - lastPeerDiscoveryAt >= 10.0) {
            lastPeerDiscoveryAt = now;
            async::spawn(geode::utils::web::WebRequest()
                .timeout(std::chrono::seconds(6))
                .get(roomUrl(code, "/members")),
                [code, peer](geode::utils::web::WebResponse response) {
                    if (!response.ok() || g_activeRoomCode != code || g_activePeerId != peer) return;
                    auto parsed = response.json();
                    if (!parsed || parsed.unwrap().isNull()) return;
                    for (auto const& [id, member] : parsed.unwrap()) {
                        if (id == peer) continue;
                        auto name = member["name"].asString();
                        g_peerNames[id] = name ? name.unwrap() : "Player";
                        rtcEnsurePeer(id);
                    }
                });
        }

        if (now - lastStateSendAt >= (1.0 / 60.0)) {
            lastStateSendAt = now;
            auto p1 = this->m_player1;
            auto p2 = this->m_player2;
            auto pos1 = p1->getPosition();
            auto pos2 = p2 ? p2->getPosition() : pos1;
            auto gm = GameManager::sharedState();
            std::ostringstream packet;
            packet.precision(6);
            int iconType = static_cast<int>(IconType::Cube);
            if (p1->m_isShip) iconType = static_cast<int>(IconType::Ship);
            else if (p1->m_isBall) iconType = static_cast<int>(IconType::Ball);
            else if (p1->m_isBird) iconType = static_cast<int>(IconType::Ufo);
            else if (p1->m_isDart) iconType = static_cast<int>(IconType::Wave);
            else if (p1->m_isRobot) iconType = static_cast<int>(IconType::Robot);
            else if (p1->m_isSpider) iconType = static_cast<int>(IconType::Spider);
            else if (p1->m_isSwing) iconType = static_cast<int>(IconType::Swing);
            packet << "S|" << pos1.x << "|" << pos1.y << "|" << p1->getRotation()
                   << "|" << pos2.x << "|" << pos2.y << "|" << (p2 ? p2->getRotation() : 0.f)
                   << "|" << (p2 ? 1 : 0) << "|" << this->getCurrentPercent()
                   << "|" << gm->getPlayerFrame() << "|" << iconType << "|" << gm->getPlayerColor()
                   << "|" << gm->getPlayerColor2() << "|" << (gm->m_playerGlow ? 1 : 0)
                   << "|" << static_cast<int>(this->m_level->m_levelID) << "|";
            std::string safeLevelName = this->m_level->m_levelName.c_str();
            for (auto& ch : safeLevelName) if (ch == '|' || ch == '\n' || ch == '\r') ch = ' ';
            if (safeLevelName.size() > 63) safeLevelName.resize(63);
            packet << safeLevelName;
            auto payload = packet.str();
            for (auto const& [remoteId, session] : g_rtcPeers) {
                (void)remoteId;
                auto channel = rtcGetChannel(session);
                if (channel && channel->isOpen()) channel->send(rtc::string(payload));
            }
        }

        std::deque<std::pair<std::string, std::string>> incoming;
        {
            std::lock_guard<std::mutex> lock(g_rtcMessagesMutex);
            incoming.swap(g_rtcMessages);
        }
        for (auto const& [remoteId, payload] : incoming) {
            float x = 0.f, y = 0.f, rotation = 0.f, x2 = 0.f, y2 = 0.f, rotation2 = 0.f, percent = 0.f;
            int dual = 0, icon = 1, iconType = static_cast<int>(IconType::Cube), color1 = 0, color2 = 3, glow = 0, levelId = -1;
            char remoteLevelName[64] = {};
            if (std::sscanf(payload.c_str(), "S|%f|%f|%f|%f|%f|%f|%d|%f|%d|%d|%d|%d|%d|%d|%63[^|]",
                &x, &y, &rotation, &x2, &y2, &rotation2, &dual, &percent, &icon, &iconType, &color1, &color2, &glow, &levelId, remoteLevelName) != 15) continue;
            std::string localLevelName = this->m_level->m_levelName.c_str();
            for (auto& ch : localLevelName) if (ch == '|' || ch == '\n' || ch == '\r') ch = ' ';
            if (localLevelName.size() > 63) localLevelName.resize(63);
            if (levelId != static_cast<int>(this->m_level->m_levelID) || localLevelName != remoteLevelName) continue;

            // Keep remote visuals in the exact same parent coordinate space as PlayerObject.
            auto worldLayer = this->m_player1->getParent();
            if (!worldLayer) worldLayer = this;
            auto it = g_remoteAvatars.find(remoteId);
            CCNode* avatar = it == g_remoteAvatars.end() ? nullptr : it->second;
            if (!avatar) {
                auto visual = SimplePlayer::create(icon);
                if (!visual) continue;
                visual->updatePlayerFrame(icon, static_cast<IconType>(iconType));
                visual->setScale(0.72f);
                visual->setID(("gd-rtc-remote-" + remoteId).c_str());
                visual->setAnchorPoint({0.5f, 0.5f});
                worldLayer->addChild(visual, this->m_player1->getZOrder() + 1);
                avatar = visual;
                g_remoteAvatars[remoteId] = avatar;

                auto secondary = SimplePlayer::create(icon);
                if (secondary) {
                    secondary->updatePlayerFrame(icon, static_cast<IconType>(iconType));
                    secondary->setScale(0.72f);
                    secondary->setID(("gd-rtc-remote-secondary-" + remoteId).c_str());
                    secondary->setVisible(false);
                    worldLayer->addChild(secondary, this->m_player1->getZOrder() + 1);
                    g_remoteSecondaryAvatars[remoteId] = secondary;
                }

                auto label = CCLabelBMFont::create("", "goldFont.fnt");
                if (label) {
                    label->setID(("gd-rtc-remote-name-" + remoteId).c_str());
                    label->setScale(0.38f);
                    label->setAnchorPoint({0.5f, 0.0f});
                    worldLayer->addChild(label, this->m_player1->getZOrder() + 2);
                    g_remoteNameLabels[remoteId] = label;
                }
                if (this->m_progressBar && !this->m_level->isPlatformer()) {
                    auto progressIcon = SimplePlayer::create(icon);
                    if (progressIcon) {
                        progressIcon->updatePlayerFrame(icon, static_cast<IconType>(iconType));
                        progressIcon->setScale(0.28f);
                        progressIcon->setID(("gd-rtc-progress-" + remoteId).c_str());
                        this->m_progressBar->addChild(progressIcon, 20);
                        g_remoteProgressIcons[remoteId] = progressIcon;
                    }
                }
            }
            avatar->setVisible(true);
            g_remoteAvatarTargets[remoteId] = {{x, y}, rotation};
            if (auto simple = typeinfo_cast<SimplePlayer*>(avatar)) {
                auto gm = GameManager::sharedState();
                simple->updatePlayerFrame(icon, static_cast<IconType>(iconType));
                simple->setColor(gm->colorForIdx(color1));
                simple->setSecondColor(gm->colorForIdx(color2));
                if (glow) simple->setGlowOutline(gm->colorForIdx(color2));
            }
            auto secondaryIt = g_remoteSecondaryAvatars.find(remoteId);
            if (secondaryIt != g_remoteSecondaryAvatars.end() && secondaryIt->second) {
                secondaryIt->second->setVisible(dual != 0);
                if (dual) {
                    auto secondary = typeinfo_cast<SimplePlayer*>(secondaryIt->second);
                    if (secondary) {
                        secondary->updatePlayerFrame(icon, static_cast<IconType>(iconType));
                        auto gm = GameManager::sharedState();
                        secondary->setColor(gm->colorForIdx(color1));
                        secondary->setSecondColor(gm->colorForIdx(color2));
                        if (glow) secondary->setGlowOutline(gm->colorForIdx(color2));
                    }
                    g_remoteSecondaryTargets[remoteId] = {{x2, y2}, rotation2};
                }
            }
            if (auto name = g_remoteNameLabels[remoteId]) {
                auto nameIt = g_peerNames.find(remoteId);
                std::string text = nameIt == g_peerNames.end() ? "Player" : nameIt->second;
                if (text.size() > 16) text.resize(16);
                name->setString(text.c_str());
            }
            auto progressIt = g_remoteProgressIcons.find(remoteId);
            if (progressIt != g_remoteProgressIcons.end() && progressIt->second && this->m_progressBar) {
                bool classic = !this->m_level->isPlatformer();
                progressIt->second->setVisible(classic);
                if (classic) {
                    auto width = this->m_progressBar->getContentSize().width;
                    progressIt->second->setPosition({width * (std::clamp(percent, 0.f, 100.f) / 100.f - 0.5f), -11.f});
                    if (auto simple = typeinfo_cast<SimplePlayer*>(progressIt->second)) {
                        simple->updatePlayerFrame(icon, static_cast<IconType>(iconType));
                        auto gm = GameManager::sharedState();
                        simple->setColor(gm->colorForIdx(color1));
                        simple->setSecondColor(gm->colorForIdx(color2));
                    }
                }
            }
            g_remoteAvatarLastSeen[remoteId] = now;
        }

        // Smooth over packet jitter instead of teleporting the remote icon for
        // every packet. The second icon and nameplate are independent world-layer
        // siblings so dual mode and rotation cannot drag/rotate the nameplate.
        for (auto const& [id, target] : g_remoteAvatarTargets) {
            auto it = g_remoteAvatars.find(id);
            if (it == g_remoteAvatars.end() || !it->second) continue;
            auto avatar = it->second;
            auto current = avatar->getPosition();
            avatar->setPosition({
                current.x + (target.first.x - current.x) * 0.42f,
                current.y + (target.first.y - current.y) * 0.42f
            });
            auto rotation = avatar->getRotation();
            auto delta = std::fmod(target.second - rotation + 540.f, 360.f) - 180.f;
            avatar->setRotation(rotation + delta * 0.42f);
            auto name = g_remoteNameLabels.find(id);
            if (name != g_remoteNameLabels.end() && name->second) {
                name->second->setPosition({avatar->getPositionX(), avatar->getPositionY() + 24.f});
            }
        }
        for (auto const& [id, target] : g_remoteSecondaryTargets) {
            auto it = g_remoteSecondaryAvatars.find(id);
            if (it == g_remoteSecondaryAvatars.end() || !it->second || !it->second->isVisible()) continue;
            auto avatar = it->second;
            auto current = avatar->getPosition();
            avatar->setPosition({
                current.x + (target.first.x - current.x) * 0.42f,
                current.y + (target.first.y - current.y) * 0.42f
            });
            auto rotation = avatar->getRotation();
            auto delta = std::fmod(target.second - rotation + 540.f, 360.f) - 180.f;
            avatar->setRotation(rotation + delta * 0.42f);
        }
        for (auto it = g_remoteAvatars.begin(); it != g_remoteAvatars.end();) {
            auto last = g_remoteAvatarLastSeen.find(it->first);
            if (last == g_remoteAvatarLastSeen.end() || now - last->second > 2.0) {
                auto id = it->first;
                if (it->second) it->second->removeFromParent();
                auto secondary = g_remoteSecondaryAvatars.find(id);
                if (secondary != g_remoteSecondaryAvatars.end()) {
                    if (secondary->second) secondary->second->removeFromParent();
                    g_remoteSecondaryAvatars.erase(secondary);
                }
                auto label = g_remoteNameLabels.find(id);
                if (label != g_remoteNameLabels.end()) {
                    if (label->second) label->second->removeFromParent();
                    g_remoteNameLabels.erase(label);
                }
                auto progress = g_remoteProgressIcons.find(id);
                if (progress != g_remoteProgressIcons.end()) {
                    if (progress->second) progress->second->removeFromParent();
                    g_remoteProgressIcons.erase(progress);
                }
                g_remoteAvatarTargets.erase(id);
                g_remoteSecondaryTargets.erase(id);
                g_remoteAvatarLastSeen.erase(id);
                it = g_remoteAvatars.erase(it);
            } else ++it;
        }
    }

    void onExit() {
        PlayLayer::onExit();
        clearRemoteAvatars();
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

        auto room = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("GD-RTC", 108, true, "bigFont.fnt", "GJ_button_01.png", 30.f, .62f),
            this, menu_selector(GDRTCPauseLayer::onOpenRoomMenu));
        room->setPosition({0.f, 18.f});
        menu->addChild(room);
    }

    void keyBackClicked() {
        if (this->getChildByID("gd-rtc-pause-room-panel")) {
            onCloseRoomMenu(nullptr);
            return;
        }
        PauseLayer::keyBackClicked();
    }

    void setPauseMenusEnabled(CCNode* node, bool enabled, CCNode* skip) {
        if (!node || node == skip) return;
        if (auto menu = typeinfo_cast<CCMenu*>(node)) menu->setTouchEnabled(enabled);
        auto children = node->getChildren();
        if (!children) return;
        for (unsigned int i = 0; i < children->count(); ++i) {
            setPauseMenusEnabled(typeinfo_cast<CCNode*>(children->objectAtIndex(i)), enabled, skip);
        }
    }

    void setPauseRoomStatus(std::string const& message) {
        if (auto panel = this->getChildByID("gd-rtc-pause-room-panel")) {
            if (auto label = typeinfo_cast<CCLabelBMFont*>(panel->getChildByID("gd-rtc-pause-room-status"))) label->setString(message.c_str());
        }
    }

    void updatePauseRoomControls() {
        auto panel = this->getChildByID("gd-rtc-pause-room-panel");
        if (!panel) return;
        bool inRoom = !g_activeRoomCode.empty();
        if (auto menu = typeinfo_cast<CCMenu*>(panel->getChildByID("gd-rtc-pause-room-actions"))) {
            if (auto node = menu->getChildByID("gd-rtc-pause-create")) node->setVisible(!inRoom);
            if (auto node = menu->getChildByID("gd-rtc-pause-join")) node->setVisible(!inRoom);
            if (auto node = menu->getChildByID("gd-rtc-pause-leave")) node->setVisible(inRoom);
        }
        if (auto input = typeinfo_cast<geode::TextInput*>(panel->getChildByID("gd-rtc-pause-code-input"))) {
            input->setVisible(!inRoom);
            input->setEnabled(!inRoom);
        }
        if (auto code = typeinfo_cast<CCLabelBMFont*>(panel->getChildByID("gd-rtc-pause-code-label"))) {
            code->setVisible(inRoom);
            if (inRoom) code->setString(("Room " + g_activeRoomCode).c_str());
        }
        for (auto id : {"gd-rtc-pause-players-title", "gd-rtc-pause-self-name", "gd-rtc-pause-self-state", "gd-rtc-pause-member-menu", "gd-rtc-pause-members-back"}) {
            if (auto node = panel->getChildByID(id)) node->setVisible(inRoom);
        }
        if (auto subtitle = typeinfo_cast<CCLabelBMFont*>(panel->getChildByID("gd-rtc-pause-subtitle"))) {
            subtitle->setString(inRoom ? "Invite friends with this room code" : "Create a room or join with a 5-digit code");
        }
    }

    void onOpenRoomMenu(CCObject*) {
        if (this->getChildByID("gd-rtc-pause-room-panel")) return;
        setPauseMenusEnabled(this, false, nullptr);
        this->setKeypadEnabled(true);
        auto win = CCDirector::sharedDirector()->getWinSize();
        auto shade = CCLayerColor::create({0, 0, 0, 190});
        shade->setID("gd-rtc-pause-room-shade");
        shade->setContentSize(win);
        this->addChild(shade, 1998);
        auto panel = CCNode::create();
        panel->setID("gd-rtc-pause-room-panel");
        panel->setPosition({win.width / 2, win.height / 2});
        panel->setScale(std::max(0.55f, std::min(1.0f, std::min((win.width - 24.f) / 360.f, (win.height - 24.f) / 420.f))));
        this->addChild(panel, 1999);
        addGeodePanelBackground(panel);
        auto title = CCLabelBMFont::create("GD-RTC", "bigFont.fnt");
        title->setScale(0.86f);
        title->setPosition({0, 171.f});
        panel->addChild(title);
        auto subtitle = CCLabelBMFont::create("Create a room or join with a 5-digit code", "goldFont.fnt");
        subtitle->setID("gd-rtc-pause-subtitle");
        subtitle->setScale(0.42f);
        subtitle->setPosition({0, 143.f});
        panel->addChild(subtitle);
        auto input = geode::TextInput::create(100.f, "00000");
        input->setID("gd-rtc-pause-code-input");
        input->setCommonFilter(geode::CommonFilter::Uint);
        input->setMaxCharCount(5);
        input->setTextAlign(geode::TextInputAlign::Center);
        input->setPosition({48.f, -15.f});
        panel->addChild(input);
        auto code = CCLabelBMFont::create("", "bigFont.fnt");
        code->setID("gd-rtc-pause-code-label");
        code->setScale(0.72f);
        code->setPosition({0, 111.f});
        code->setVisible(false);
        panel->addChild(code);
        auto membersBack = CCLayerColor::create({69, 44, 29, 220});
        membersBack->setID("gd-rtc-pause-members-back");
        membersBack->setContentSize({320.f, 218.f});
        membersBack->setPosition({-160.f, -151.f});
        panel->addChild(membersBack);
        auto playersTitle = CCLabelBMFont::create("Players", "goldFont.fnt");
        playersTitle->setID("gd-rtc-pause-players-title");
        playersTitle->setScale(0.48f);
        playersTitle->setPosition({-126.f, 72.f});
        playersTitle->setAnchorPoint({0.f, 0.5f});
        panel->addChild(playersTitle);
        auto selfName = CCLabelBMFont::create("", "goldFont.fnt");
        selfName->setID("gd-rtc-pause-self-name");
        selfName->setAnchorPoint({0.f, 0.5f});
        selfName->setScale(0.46f);
        selfName->setPosition({-126.f, 48.f});
        panel->addChild(selfName);
        auto selfState = CCLabelBMFont::create("", "goldFont.fnt");
        selfState->setID("gd-rtc-pause-self-state");
        selfState->setAnchorPoint({0.f, 0.5f});
        selfState->setScale(0.34f);
        selfState->setPosition({-116.f, 34.f});
        panel->addChild(selfState);
        auto memberMenu = CCMenu::create();
        memberMenu->setID("gd-rtc-pause-member-menu");
        memberMenu->setPosition({0, 0});
        panel->addChild(memberMenu);
        auto status = CCLabelBMFont::create("Ready", "goldFont.fnt");
        status->setID("gd-rtc-pause-room-status");
        status->setScale(0.38f);
        status->setPosition({0, -194.f});
        panel->addChild(status);
        auto actions = CCMenu::create();
        actions->setID("gd-rtc-pause-room-actions");
        actions->setPosition({0, 25.f});
        panel->addChild(actions);
        auto create = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Create Room", 120, true, "bigFont.fnt", "GJ_button_01.png", 30.f, .65f),
            this, menu_selector(GDRTCPauseLayer::onPauseCreateRoom));
        create->setID("gd-rtc-pause-create");
        create->setPosition({0.f, 0.f});
        actions->addChild(create);
        auto join = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Join", 72, true, "bigFont.fnt", "GJ_button_01.png", 30.f, .65f),
            this, menu_selector(GDRTCPauseLayer::onPauseJoinRoom));
        join->setID("gd-rtc-pause-join");
        join->setPosition({-76.f, -40.f});
        actions->addChild(join);
        auto leave = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Leave Room", 120, true, "bigFont.fnt", "GJ_button_04.png", 30.f, .62f),
            this, menu_selector(GDRTCPauseLayer::onPauseLeaveRoom));
        leave->setID("gd-rtc-pause-leave");
        leave->setPosition({0.f, -164.f});
        actions->addChild(leave);
        auto closeSprite = CCSprite::createWithSpriteFrameName("GJ_closeBtn_001.png");
        auto close = CCMenuItemSpriteExtra::create(closeSprite, this, menu_selector(GDRTCPauseLayer::onCloseRoomMenu));
        auto closeMenu = CCMenu::create(close, nullptr);
        closeMenu->setPosition({0, 0});
        panel->addChild(closeMenu, 20);
        close->setPosition({-160.f, 190.f});
        close->setScale(0.72f);
        updatePauseRoomControls();
        this->schedule(schedule_selector(GDRTCPauseLayer::pollPauseRoom), 10.f);
        this->schedule(schedule_selector(GDRTCPauseLayer::pollPauseSignaling), 1.f);
        if (!g_activeRoomCode.empty()) onPauseRefreshMembers(nullptr);
    }

    void onCloseRoomMenu(CCObject*) {
        this->unschedule(schedule_selector(GDRTCPauseLayer::pollPauseRoom));
        this->unschedule(schedule_selector(GDRTCPauseLayer::pollPauseSignaling));
        if (auto panel = this->getChildByID("gd-rtc-pause-room-panel")) panel->removeFromParent();
        if (auto shade = this->getChildByID("gd-rtc-pause-room-shade")) shade->removeFromParent();
        setPauseMenusEnabled(this, true, nullptr);
    }

    void pollPauseSignaling(float) {
        if (!g_activeRoomCode.empty()) rtcPollSignals();
    }

    void pollPauseRoom(float) {
        if (!this->getChildByID("gd-rtc-pause-room-panel")) {
            this->unschedule(schedule_selector(GDRTCPauseLayer::pollPauseRoom));
            this->unschedule(schedule_selector(GDRTCPauseLayer::pollPauseSignaling));
            return;
        }
        if (!g_activeRoomCode.empty()) onPauseRefreshMembers(nullptr);
    }

    void onPauseCreateRoom(CCObject*) {
        if (g_roomOperationPending) return;
        g_roomOperationPending = true;
        setPauseRoomStatus("Creating room...");
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
        Ref<GDRTCPauseLayer> self = this;
        async::spawn(geode::utils::web::WebRequest().timeout(std::chrono::seconds(12))
            .bodyJSON(room).put(roomUrl(code)),
            [self, code, peer](geode::utils::web::WebResponse response) {
                g_roomOperationPending = false;
                if (!response.ok()) { self->setPauseRoomStatus("Couldn't create room. Try again."); return; }
                g_activeRoomCode = code;
                g_activePeerId = peer;
                self->updatePauseRoomControls();
                self->setPauseRoomStatus("Room created.");
                self->onPauseRefreshMembers(nullptr);
            });
    }

    void onPauseJoinRoom(CCObject*) {
        if (g_roomOperationPending) return;
        auto panel = this->getChildByID("gd-rtc-pause-room-panel");
        auto input = panel ? typeinfo_cast<geode::TextInput*>(panel->getChildByID("gd-rtc-pause-code-input")) : nullptr;
        if (!input) return;
        std::string code(input->getString().c_str());
        if (code.size() != 5 || !std::all_of(code.begin(), code.end(), [](unsigned char ch) { return std::isdigit(ch); })) {
            setPauseRoomStatus("Enter a 5-digit room code.");
            return;
        }
        g_roomOperationPending = true;
        setPauseRoomStatus("Joining room...");
        auto peer = makePeerId();
        auto now = nowSeconds();
        Ref<GDRTCPauseLayer> self = this;
        async::spawn(geode::utils::web::WebRequest().timeout(std::chrono::seconds(10))
            .get(roomUrl(code, "/members")),
            [self, code, peer, now](geode::utils::web::WebResponse response) {
                if (!response.ok()) { g_roomOperationPending = false; self->setPauseRoomStatus("Room not found."); return; }
                auto parsed = response.json();
                if (!parsed || parsed.unwrap().isNull()) { g_roomOperationPending = false; self->setPauseRoomStatus("Room not found."); return; }
                unsigned int count = 0;
                for (auto const& [id, value] : parsed.unwrap()) { (void)id; (void)value; ++count; }
                if (count >= 8) { g_roomOperationPending = false; self->setPauseRoomStatus("Room is full."); return; }
                matjson::Value member = matjson::Value::object();
                member["name"] = playerName();
                member["peerId"] = peer;
                member["joinedAt"] = now;
                member["lastSeen"] = now;
                async::spawn(geode::utils::web::WebRequest().timeout(std::chrono::seconds(10))
                    .bodyJSON(member).put(roomUrl(code, "/members/" + peer)),
                    [self, code, peer](geode::utils::web::WebResponse joined) {
                        g_roomOperationPending = false;
                        if (!joined.ok()) { self->setPauseRoomStatus("Couldn't join room."); return; }
                        g_activeRoomCode = code;
                        g_activePeerId = peer;
                        self->updatePauseRoomControls();
                        self->setPauseRoomStatus("Joined room.");
                        self->onPauseRefreshMembers(nullptr);
                    });
            });
    }

    void onPauseLeaveRoom(CCObject*) {
        if (g_activeRoomCode.empty() || g_activePeerId.empty()) return;
        auto code = g_activeRoomCode;
        auto peer = g_activePeerId;
        Ref<GDRTCPauseLayer> self = this;
        async::spawn(geode::utils::web::WebRequest().timeout(std::chrono::seconds(8))
            .send("DELETE", roomUrl(code, "/members/" + peer)),
            [self, code, peer](geode::utils::web::WebResponse response) {
                if (!response.ok()) { self->setPauseRoomStatus("Couldn't leave room."); return; }
                rtcCloseAllPeers();
                clearRemoteAvatars();
                g_activeRoomCode.clear();
                g_activePeerId.clear();
                g_peerNames.clear();
                g_roomMemberIds.clear();
                self->updatePauseRoomControls();
                self->setPauseRoomStatus("Left room.");
                async::spawn(geode::utils::web::WebRequest().timeout(std::chrono::seconds(6))
                    .get(roomUrl(code)),
                    [code](geode::utils::web::WebResponse roomResponse) {
                        if (!roomResponse.ok()) return;
                        auto parsed = roomResponse.json();
                        if (!parsed || parsed.unwrap().isNull()) return;
                        bool empty = true;
                        for (auto const& [id, value] : parsed.unwrap()["members"]) { (void)id; (void)value; empty = false; break; }
                        if (empty) async::spawn(geode::utils::web::WebRequest().timeout(std::chrono::seconds(6))
                            .send("DELETE", roomUrl(code)), [](geode::utils::web::WebResponse) {});
                    });
            });
    }

    void onPauseRefreshMembers(CCObject*) {
        if (g_refreshPending || g_activeRoomCode.empty()) return;
        g_refreshPending = true;
        auto code = g_activeRoomCode;
        Ref<GDRTCPauseLayer> self = this;
        async::spawn(geode::utils::web::WebRequest().timeout(std::chrono::seconds(8))
            .get(roomUrl(code, "/members")),
            [self, code](geode::utils::web::WebResponse response) {
                g_refreshPending = false;
                if (!response.ok() || g_activeRoomCode != code) return;
                auto parsed = response.json();
                if (!parsed || parsed.unwrap().isNull()) { self->setPauseRoomStatus("Room no longer exists."); return; }
                auto members = parsed.unwrap();
                auto panel = self->getChildByID("gd-rtc-pause-room-panel");
                if (!panel) return;
                unsigned int totalMembers = 0;
                for (auto const& [memberId, memberValue] : members) { (void)memberId; (void)memberValue; ++totalMembers; }
                if (auto title = typeinfo_cast<CCLabelBMFont*>(panel->getChildByID("gd-rtc-pause-players-title"))) title->setString(("Players (" + std::to_string(totalMembers) + "/8)").c_str());
                if (auto own = typeinfo_cast<CCLabelBMFont*>(panel->getChildByID("gd-rtc-pause-self-name"))) own->setString((playerName() + " (you)").c_str());
                if (auto ownState = typeinfo_cast<CCLabelBMFont*>(panel->getChildByID("gd-rtc-pause-self-state"))) {
                    auto play = PlayLayer::get();
                    if (play && play->m_level) {
                        auto name = std::string(play->m_level->m_levelName.c_str());
                        if (name.size() > 28) name.resize(28);
                        auto levelID = static_cast<int>(play->m_level->m_levelID);
                        auto levelTypeValue = static_cast<int>(play->m_level->m_levelType);
                        std::string typeName = levelTypeValue == 1 ? "main" : (levelTypeValue == 4 ? "online" : "custom");
                        ownState->setString(("PLAYING " + name + " (" + std::to_string(levelID) + ") [" + typeName + "]").c_str());
                    } else ownState->setString("");
                }
                if (auto menu = typeinfo_cast<CCMenu*>(panel->getChildByID("gd-rtc-pause-member-menu"))) {
                    menu->removeAllChildren();
                    g_roomMemberIds.clear();
                    unsigned int row = 0;
                    for (auto const& [id, member] : members) {
                        if (id == g_activePeerId) continue;
                        if (row >= 7) break;
                        auto nameValue = member["name"].asString();
                        std::string name = nameValue ? nameValue.unwrap() : "Player";
                        auto state = member["currentLevel"];
                        auto level = state["name"].asString();
                        auto levelId = state["levelID"].asInt();
                        auto type = state["levelType"].asString();
                        auto updated = state["updatedAt"].asDouble();
                        bool playing = level && levelId && updated && nowSeconds() - updated.unwrap() < 35.0;
                        std::string detail;
                        if (playing) {
                            auto ln = level.unwrap();
                            if (ln.size() > 26) ln.resize(26);
                            detail = "PLAYING " + ln + " (" + std::to_string(levelId.unwrap()) + ")";
                            if (type) detail += " [" + type.unwrap() + "]";
                        }
                        g_roomMemberIds.push_back(id);
                        auto nameLabel = CCLabelBMFont::create(name.c_str(), "goldFont.fnt");
                        nameLabel->setAnchorPoint({0.f, 0.5f});
                        nameLabel->setScale(0.34f);
                        nameLabel->setPosition({-126.f, 24.f - row * 22.f});
                        menu->addChild(nameLabel);
                        auto detailLabel = CCLabelBMFont::create(detail.c_str(), "goldFont.fnt");
                        detailLabel->setAnchorPoint({0.f, 0.5f});
                        detailLabel->setScale(0.25f);
                        detailLabel->setPosition({-116.f, 14.f - row * 22.f});
                        menu->addChild(detailLabel);
                        auto join = CCMenuItemSpriteExtra::create(
                            ButtonSprite::create("Join", 68, true, "bigFont.fnt", playing ? "GJ_button_01.png" : "GJ_button_04.png", 22.f, .58f),
                            self, menu_selector(GDRTCPauseLayer::onPauseJoinPlayer));
                        join->setTag(static_cast<int>(g_roomMemberIds.size() - 1));
                        join->setPosition({119.f, 20.f - row * 22.f});
                        join->setEnabled(playing);
                        menu->addChild(join);
                        rtcEnsurePeer(id);
                        if (nameValue) g_peerNames[id] = nameValue.unwrap();
                        ++row;
                    }
                }
                self->setPauseRoomStatus("Room " + code);
            });
    }

    void onPauseJoinPlayer(CCObject* sender) {
        auto item = typeinfo_cast<CCMenuItemSpriteExtra*>(sender);
        if (!item || g_activeRoomCode.empty()) return;
        auto index = item->getTag();
        if (index < 0 || static_cast<size_t>(index) >= g_roomMemberIds.size()) return;
        auto code = g_activeRoomCode;
        auto peer = g_roomMemberIds[static_cast<size_t>(index)];
        Ref<GDRTCPauseLayer> self = this;
        async::spawn(geode::utils::web::WebRequest().timeout(std::chrono::seconds(12))
            .get(roomUrl(code, "/levels/" + peer)),
            [self, code](geode::utils::web::WebResponse response) {
                if (!response.ok() || g_activeRoomCode != code) { self->setPauseRoomStatus("Level isn't available yet."); return; }
                auto parsed = response.json();
                if (!parsed || parsed.unwrap().isNull()) { self->setPauseRoomStatus("Level isn't available yet."); return; }
                auto item = parsed.unwrap();
                auto nameValue = item["name"].asString();
                auto dataValue = item["levelData"].asString();
                auto idValue = item["levelID"].asInt();
                auto typeValue = item["levelType"].asString();
                std::string name = nameValue ? nameValue.unwrap() : "Shared level";
                std::string data = dataValue ? dataValue.unwrap() : "";
                GJGameLevel* level = nullptr;
                if (typeValue && typeValue.unwrap() == "main" && idValue)
                    level = GameLevelManager::sharedState()->getMainLevel(static_cast<int>(idValue.unwrap()), false);
                if (!level && !data.empty()) {
                    level = GJGameLevel::create();
                    level->m_levelName = name.c_str();
                    level->m_levelString = data.c_str();
                    level->m_levelID = idValue ? static_cast<int>(idValue.unwrap()) : 0;
                }
                if (!level) { self->setPauseRoomStatus("Level data isn't available."); return; }
                auto scene = PlayLayer::scene(level, false, false);
                if (scene) CCDirector::sharedDirector()->pushScene(scene);
            });
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
