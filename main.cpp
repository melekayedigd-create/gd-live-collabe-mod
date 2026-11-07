// Live Collab — build levels together with a friend in real time.
//
//  Main menu:  [Host]  -> creates a room and shows a 6-letter code
//              [Join]  -> type your friend's code; the host's level opens
//                         automatically in the editor (same song, colors...)
//  Editor:     live object sync, a movable chat button, and boxes + names
//              around what the other player is selecting / building.

#include <Geode/Geode.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/TextInput.hpp>
#include <Geode/ui/Notification.hpp>
#include <chrono>

#include "Collab.hpp"

using namespace geode::prelude;

namespace collab {

Globals& G() {
    static Globals g;
    return g;
}

double now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

std::string myName() {
    std::string name = GameManager::get()->m_playerName;
    if (name.empty()) name = "Player";
    return name;
}

std::string nameOf(std::string const& sender) {
    auto it = G().players.find(sender);
    return it == G().players.end() ? std::string("Friend") : it->second.name;
}

cocos2d::ccColor4F colorFor(std::string const& sender) {
    static const cocos2d::ccColor4F palette[] = {
        { 1.f, .35f, .35f, 1.f }, { .3f, .75f, 1.f, 1.f }, { 1.f, .8f, .2f, 1.f },
        { .45f, 1.f, .45f, 1.f }, { .85f, .45f, 1.f, 1.f }, { 1.f, .55f, .15f, 1.f },
    };
    size_t h = std::hash<std::string>{}(sender);
    return palette[h % 6];
}

void addChatLine(std::string const& name, std::string const& text, bool mine) {
    auto& chat = G().chat;
    chat.push_back({ name, text, mine });
    if (chat.size() > 60) chat.erase(chat.begin(), chat.begin() + (chat.size() - 60));
}

// "2\n<audioTrack>\n<songID>\n<songIDs>\n<sfxIDs>\n<compressed level>"
std::string encodeFull(FullLevel const& l) {
    return "2\n" + std::to_string(l.audioTrack) + "\n" + std::to_string(l.songID) + "\n" +
        l.songIDs + "\n" + l.sfxIDs + "\n" + l.compressed;
}

bool decodeFull(std::string const& body, FullLevel& out) {
    std::vector<std::string> parts;
    size_t start = 0;
    for (int i = 0; i < 5; i++) {
        size_t nl = body.find('\n', start);
        if (nl == std::string::npos) return false;
        parts.push_back(body.substr(start, nl - start));
        start = nl + 1;
    }
    if (parts[0] != "2") return false;
    out.audioTrack = std::atoi(parts[1].c_str());
    out.songID = std::atoi(parts[2].c_str());
    out.songIDs = parts[3];
    out.sfxIDs = parts[4];
    out.compressed = body.substr(start);
    return true;
}

static void closeWaitingPopup() {
    auto& g = G();
    if (g.waitingPopup) {
        g.closingWaiting = true;
        if (g.waitingPopup->getParent()) g.waitingPopup->keyBackClicked();
        g.closingWaiting = false;
        g.waitingPopup = nullptr;
    }
}

// Guest: create (or reuse) a level for this room and open it in the editor
static void openCollabLevel(FullLevel const& full) {
    auto& g = G();
    auto room = Session::get().room();
    GJGameLevel* level = g.collabLevel;
    if (!level || g.collabRoom != room) {
        level = GameLevelManager::get()->createNewLevel();
        if (!level) return;
        g.collabLevel = level;
        g.collabRoom = room;
    }
    std::string title = "Collab " + room;
    level->m_levelName = title;
    level->m_levelString = full.compressed;
    level->m_audioTrack = full.audioTrack;
    level->m_songID = full.songID;
    level->m_songIDs = full.songIDs;
    level->m_sfxIDs = full.sfxIDs;

    g.freshLevel = true;
    closeWaitingPopup();
    CCDirector::get()->replaceScene(CCTransitionFade::create(.5f, LevelEditorLayer::scene(level, false)));
}

static void handleFullLevel(Message&& msg) {
    FullLevel full;
    if (!decodeFull(msg.body, full)) return;
    auto& g = G();
    if (LevelEditorLayer::get()) {
        // Already in the editor: the editor replaces its objects in place
        g.editorQueue.push_back(std::move(msg));
        closeWaitingPopup();
        return;
    }
    if (PlayLayer::get()) {
        // Playing a level right now: open the editor when they leave it
        g.pendingFull = std::move(full);
        g.hasPendingFull = true;
        Notification::create("Your friend's level is ready - exit this level to join", NotificationIcon::Info, 3.f)->show();
        return;
    }
    openCollabLevel(full);
}

// Runs all the time (menu, editor, anywhere): receives messages and shows events
class CollabPump : public CCObject {
public:
    void tick(float) {
        auto& s = Session::get();
        auto& g = G();

        for (auto const& text : s.drainNotices()) {
            bool bad = text.find("failed") != std::string::npos || text.find("lost") != std::string::npos;
            Notification::create(text, bad ? NotificationIcon::Error : NotificationIcon::Info, 2.5f)->show();
        }

        if (!s.active()) {
            g.players.clear();
            g.editorQueue.clear();
            g.flashes.clear();
            g.hasPendingFull = false;
            return;
        }

        bool inEditor = LevelEditorLayer::get() != nullptr;
        for (auto& msg : s.drain()) {
            auto& player = g.players[msg.sender];
            player.lastSeen = now();
            switch (msg.type) {
                case 'J':
                case 'W':
                    if (!msg.body.empty()) player.name = msg.body;
                    if (msg.type == 'J' && inEditor) g.editorQueue.push_back(std::move(msg));
                    break;
                case 'B':
                    g.players.erase(msg.sender);
                    break;
                case 'C': {
                    auto nl = msg.body.find('\n');
                    if (nl == std::string::npos) break;
                    player.name = msg.body.substr(0, nl);
                    auto text = msg.body.substr(nl + 1);
                    addChatLine(player.name, text, false);
                    if (!chatIsShowing()) {
                        g.unread++;
                        Notification::create(player.name + ": " + text, NotificationIcon::None, 2.5f)->show();
                    }
                    break;
                }
                case 'S': {
                    auto nl = msg.body.find('\n');
                    if (nl == std::string::npos) break;
                    player.name = msg.body.substr(0, nl);
                    player.selection.clear();
                    size_t pos = nl + 1;
                    while (pos < msg.body.size()) {
                        size_t end = msg.body.find(';', pos);
                        if (end == std::string::npos) end = msg.body.size();
                        float x, y, w, h;
                        if (std::sscanf(msg.body.c_str() + pos, "%f,%f,%f,%f", &x, &y, &w, &h) == 4) {
                            player.selection.push_back(CCRect(x, y, w, h));
                        }
                        pos = end + 1;
                    }
                    break;
                }
                case 'F':
                    if (!s.isHost()) handleFullLevel(std::move(msg));
                    break;
                case 'D':
                case 'R':
                    if (inEditor) g.editorQueue.push_back(std::move(msg));
                    break;
                default:
                    break;
            }
        }

        // Forget players we haven't heard from in a while
        for (auto it = g.players.begin(); it != g.players.end();) {
            if (now() - it->second.lastSeen > 30.0) it = g.players.erase(it);
            else ++it;
        }
        if (!inEditor) {
            g.editorQueue.clear();
            g.flashes.clear();
            if (g.hasPendingFull && !PlayLayer::get()) {
                g.hasPendingFull = false;
                openCollabLevel(g.pendingFull);
            }
        }
    }
};

void ensurePump() {
    static CollabPump* pump = nullptr;
    if (pump) return;
    pump = new CollabPump(); // lives for the whole game session
    CCDirector::get()->getScheduler()->scheduleSelector(schedule_selector(CollabPump::tick), pump, 0.1f, false);
}

} // namespace collab

using namespace collab;

static void askLeaveRoom() {
    auto& s = Session::get();
    createQuickPopup(
        "Live Collab",
        fmt::format("You are in room <cy>{}</c>.\nDo you want to leave it?", s.room()),
        "Stay", "Leave",
        [](FLAlertLayer*, bool leave) {
            if (leave) {
                Session::get().leave();
                Notification::create("You left the room", NotificationIcon::Info, 2.f)->show();
            }
        }
    );
}

static void showWaitingPopup(std::string const& code) {
    auto popup = createQuickPopup(
        "Joining...",
        fmt::format(
            "Room <cy>{}</c>\n\nWaiting for your friend to open a level in the <cg>editor</c>.\n"
            "It will open here automatically.",
            code
        ),
        "Cancel", nullptr,
        [](FLAlertLayer*, bool) {
            if (G().closingWaiting) return;
            G().waitingPopup = nullptr;
            Session::get().leave();
        }
    );
    G().waitingPopup = popup;
}

// ---------------------------------------------------------------------------
// "Join a room" popup
// ---------------------------------------------------------------------------
class JoinPopup : public Popup {
protected:
    TextInput* m_input = nullptr;

    bool init() {
        if (!Popup::init(270.f, 170.f)) return false;
        this->setTitle("Join a Room");

        auto info = CCLabelBMFont::create("Type the code your friend sent you", "chatFont.fnt");
        info->setScale(.6f);
        m_mainLayer->addChildAtPosition(info, Anchor::Top, ccp(0, -45));

        m_input = TextInput::create(170.f, "ABC123");
        m_input->setMaxCharCount(8);
        m_input->setFilter("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789- ");
        m_mainLayer->addChildAtPosition(m_input, Anchor::Center, ccp(0, 5));

        auto pasteBtn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Paste", "goldFont.fnt", "GJ_button_04.png", .55f),
            this, menu_selector(JoinPopup::onPaste)
        );
        m_buttonMenu->addChildAtPosition(pasteBtn, Anchor::Center, ccp(0, -30));

        auto joinBtn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Join", "goldFont.fnt", "GJ_button_01.png", .8f),
            this, menu_selector(JoinPopup::onJoin)
        );
        m_buttonMenu->addChildAtPosition(joinBtn, Anchor::Bottom, ccp(0, 24));
        return true;
    }

    void onPaste(CCObject*) {
        auto code = Session::normalizeCode(clipboard::read());
        if (code.size() > 8) code = code.substr(0, 8);
        m_input->setString(code);
    }

    void onJoin(CCObject*) {
        auto code = Session::normalizeCode(m_input->getString());
        if (code.size() != 6) {
            FLAlertLayer::create("Oops", "A room code has <cy>6</c> letters/numbers.\nAsk your friend to send it again.", "OK")->show();
            return;
        }
        Session::get().setName(myName());
        Session::get().join(code);
        this->onClose(nullptr);
        showWaitingPopup(code);
    }

public:
    static JoinPopup* create() {
        auto ret = new JoinPopup();
        if (ret->init()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

// ---------------------------------------------------------------------------
// Main menu: the two buttons
// ---------------------------------------------------------------------------
class $modify(CollabMenuLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;
        ensurePump();

        auto hostBtn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Host", "goldFont.fnt", "GJ_button_01.png", .7f),
            this, menu_selector(CollabMenuLayer::onCollabHost)
        );
        hostBtn->setID("host-button"_spr);

        auto joinBtn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Join", "goldFont.fnt", "GJ_button_02.png", .7f),
            this, menu_selector(CollabMenuLayer::onCollabJoin)
        );
        joinBtn->setID("join-button"_spr);

        auto win = CCDirector::get()->getWinSize();
        auto menu = CCMenu::create();
        menu->setID("live-collab-menu"_spr);
        menu->setContentSize({ 70.f, 75.f });
        menu->addChild(hostBtn);
        menu->addChild(joinBtn);
        menu->setLayout(ColumnLayout::create()->setGap(6.f)->setAxisReverse(true)->setAutoScale(true));
        menu->setPosition({ 42.f, win.height / 2.f + 10.f });
        this->addChild(menu, 20);

        auto title = CCLabelBMFont::create("Live Collab", "bigFont.fnt");
        title->setScale(.3f);
        title->setPosition({ 42.f, win.height / 2.f + 58.f });
        title->setID("live-collab-title"_spr);
        this->addChild(title, 20);
        return true;
    }

    void onCollabHost(CCObject*) {
        auto& s = Session::get();
        if (s.active()) { askLeaveRoom(); return; }
        s.setName(myName());
        s.host();
        auto code = s.room();
        clipboard::write(code);
        FLAlertLayer::create(
            "Room created!",
            fmt::format(
                "Your room code is\n<cy>{}</c>\n(copied - paste it in Messenger)\n\n"
                "Send it to your friend, then open the level you want to build in the <cg>editor</c>.",
                code
            ),
            "OK"
        )->show();
    }

    void onCollabJoin(CCObject*) {
        if (Session::get().active()) { askLeaveRoom(); return; }
        if (auto popup = JoinPopup::create()) popup->show();
    }
};
