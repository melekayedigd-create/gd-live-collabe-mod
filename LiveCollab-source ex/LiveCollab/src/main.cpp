// Live Collab — build levels together with a friend in real time.
//
//  Main menu:  [Host]  -> creates a room and shows a 6-letter code
//              [Join]  -> type your friend's code
//  Then both players open a level in the editor. The host's level is sent to
//  the friend, and every object placed / moved / edited / deleted by either
//  player appears on the other screen.

#include <Geode/Geode.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/LevelEditorLayer.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/TextInput.hpp>
#include <Geode/ui/Notification.hpp>
#include <Geode/utils/cocos.hpp>
#include <unordered_set>

#include "Session.hpp"
#include "SyncLogic.hpp"

using namespace geode::prelude;
using collab::Session;

// ---------------------------------------------------------------------------
// Shows connection events ("Your friend joined!") anywhere in the game
// ---------------------------------------------------------------------------
class CollabPump : public CCObject {
public:
    static void ensure() {
        static CollabPump* pump = nullptr;
        if (pump) return;
        pump = new CollabPump(); // lives for the whole game session
        CCDirector::get()->getScheduler()->scheduleSelector(
            schedule_selector(CollabPump::tick), pump, 0.3f, false
        );
    }

    void tick(float) {
        for (auto const& text : Session::get().drainNotices()) {
            bool bad = text.find("failed") != std::string::npos || text.find("lost") != std::string::npos;
            Notification::create(text, bad ? NotificationIcon::Error : NotificationIcon::Info, 2.5f)->show();
        }
    }
};

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
        Session::get().join(code);
        this->onClose(nullptr);
        FLAlertLayer::create(
            "Joining...",
            fmt::format(
                "Connecting to room <cy>{}</c>.\n\nNow <cg>create a new empty level</c> and open it in the "
                "<cg>editor</c>. Your friend's level will appear there automatically.",
                code
            ),
            "OK"
        )->show();
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
        CollabPump::ensure();

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

        // Our own small menu on the left side, so it never hides game buttons
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

// ---------------------------------------------------------------------------
// Editor: live sync of objects
// ---------------------------------------------------------------------------
class $modify(CollabEditor, LevelEditorLayer) {
    struct Fields {
        collab::Counts baseline;
        bool wasConnected = false;
        bool gotLevel = false;
        float sinceRequest = 0.f;
        CCLabelBMFont* label = nullptr;
    };

    bool init(GJGameLevel* level, bool noUI) {
        if (!LevelEditorLayer::init(level, noUI)) return false;

        auto win = CCDirector::get()->getWinSize();
        auto label = CCLabelBMFont::create("", "bigFont.fnt");
        label->setScale(.28f);
        label->setOpacity(200);
        label->setPosition({ win.width / 2.f, win.height - 8.f });
        label->setID("status-label"_spr);
        this->addChild(label, 1000);
        m_fields->label = label;

        m_fields->baseline = this->collabCounts(this->collabSnapshot());
        Session::get().clearInbox(); // changes made before we entered are covered by the full level
        this->schedule(schedule_selector(CollabEditor::collabTick), 0.4f);
        return true;
    }

    std::vector<std::pair<GameObject*, std::string>> collabSnapshot() {
        std::vector<std::pair<GameObject*, std::string>> out;
        if (!m_objects) return out;
        out.reserve(m_objects->count());
        for (auto obj : CCArrayExt<GameObject*>(m_objects)) {
            if (!obj) continue;
            std::string str = obj->getSaveString(this);
            out.emplace_back(obj, std::move(str));
        }
        return out;
    }

    collab::Counts collabCounts(std::vector<std::pair<GameObject*, std::string>> const& snap) {
        collab::Counts c;
        c.reserve(snap.size());
        for (auto const& [obj, str] : snap) c[str]++;
        return c;
    }

    void collabSendFullLevel(std::vector<std::pair<GameObject*, std::string>> const& snap) {
        std::string level;
        for (auto const& [obj, str] : snap) { level += str; level += ';'; }
        std::string packed;
        if (!level.empty()) {
            gd::string zipped = ZipUtils::compressString(level, false, 0);
            packed = zipped;
        }
        Session::get().send('F', packed);
    }

    void collabRemove(GameObject* obj) {
        if (m_editorUI && obj->m_isSelected) m_editorUI->deselectObject(obj);
        this->removeObject(obj, true);
    }

    void collabApplyFull(std::string const& packed) {
        if (m_editorUI) m_editorUI->deselectAll();
        auto snap = this->collabSnapshot();
        for (auto const& [obj, str] : snap) this->removeObject(obj, true);
        if (!packed.empty()) {
            gd::string unzipped = ZipUtils::decompressString(packed, false, 0);
            std::string level = unzipped;
            if (!level.empty()) this->createObjectsFromString(level, true, true);
        }
        if (m_editorUI) m_editorUI->updateButtons();
    }

    void collabApplyDiff(std::string const& body) {
        auto diff = collab::decodeDiff(body);
        if (diff.empty()) return;

        if (!diff.removed.empty()) {
            auto snap = this->collabSnapshot();
            std::unordered_map<std::string, std::vector<GameObject*>> exact, loose;
            for (auto const& [obj, str] : snap) {
                exact[str].push_back(obj);
                loose[collab::signatureOf(str)].push_back(obj);
            }
            std::unordered_set<GameObject*> gone;
            auto take = [&](std::vector<GameObject*>& list) -> GameObject* {
                while (!list.empty()) {
                    auto obj = list.back();
                    list.pop_back();
                    if (!gone.count(obj)) return obj;
                }
                return nullptr;
            };
            for (auto const& str : diff.removed) {
                GameObject* obj = nullptr;
                if (auto it = exact.find(str); it != exact.end()) obj = take(it->second);
                if (!obj) {
                    if (auto it = loose.find(collab::signatureOf(str)); it != loose.end()) obj = take(it->second);
                }
                if (obj) {
                    gone.insert(obj);
                    this->collabRemove(obj);
                }
            }
        }

        if (!diff.added.empty()) {
            std::string joined;
            for (auto const& s : diff.added) { joined += s; joined += ';'; }
            this->createObjectsFromString(joined, true, true);
        }
    }

    void collabTick(float dt) {
        auto& s = Session::get();
        auto& f = m_fields;
        bool connected = s.state() == collab::State::Connected;

        if (f->label) {
            if (!s.active()) f->label->setString("");
            else if (!connected) f->label->setString(fmt::format("Live Collab: connecting to room {}...", s.room()).c_str());
            else f->label->setString(fmt::format("Live Collab: room {} ({})", s.room(), s.isHost() ? "host" : "guest").c_str());
        }

        if (!connected) { f->wasConnected = false; return; }
        // Don't sync while play-testing: objects move around during the test
        if (m_playbackMode != PlaybackMode::Not) return;

        auto snap = this->collabSnapshot();

        // Just connected (or entered the editor while connected)
        if (!f->wasConnected) {
            f->wasConnected = true;
            f->sinceRequest = 0.f;
            if (s.isHost()) this->collabSendFullLevel(snap);
            else { f->gotLevel = false; s.send('R', ""); }
        }

        // 1) send what *I* changed since last time
        auto current = this->collabCounts(snap);
        auto diff = collab::computeDiff(f->baseline, current);
        if (!diff.empty()) s.send('D', collab::encodeDiff(diff));
        f->baseline = std::move(current);

        // Guest: ask again for the level if the host wasn't in the editor yet
        if (!s.isHost() && !f->gotLevel) {
            f->sinceRequest += dt;
            if (f->sinceRequest > 4.f) { f->sinceRequest = 0.f; s.send('R', ""); }
        }

        // 2) apply what my friend changed
        bool changed = false;
        for (auto& msg : s.drain()) {
            switch (msg.type) {
                case 'R':
                case 'J':
                    if (s.isHost()) this->collabSendFullLevel(snap);
                    break;
                case 'F':
                    if (!s.isHost()) {
                        this->collabApplyFull(msg.body);
                        f->gotLevel = true;
                        changed = true;
                        Notification::create("Level received from the host!", NotificationIcon::Success, 2.f)->show();
                    }
                    break;
                case 'D':
                    this->collabApplyDiff(msg.body);
                    changed = true;
                    break;
                default:
                    break;
            }
        }

        // Objects created by my friend are not "my" changes: don't send them back
        if (changed) f->baseline = this->collabCounts(this->collabSnapshot());
    }
};
