// Everything that happens inside the level editor:
//  - sends my changes, applies my friend's changes
//  - draws boxes + names around what my friend selects / builds / deletes
//  - the movable chat button
//
// Performance / stability notes (tablets):
//  - Objects are only re-serialized when they moved, are selected, are new,
//    or during a slow full check every 3 seconds (not every object every tick).
//  - Friend's changes are NOT applied while I'm touching the editor or have a
//    GD popup open (editing an object); they wait a moment instead. Removing an
//    object that GD is still using was the most likely cause of crashes.

#include <Geode/Geode.hpp>
#include <Geode/modify/LevelEditorLayer.hpp>
#include <Geode/modify/EditorUI.hpp>
#include <Geode/ui/Notification.hpp>
#include <Geode/ui/BasedButtonSprite.hpp>
#include <unordered_map>
#include <unordered_set>

#include "Collab.hpp"
#include "SyncLogic.hpp"

using namespace geode::prelude;
using namespace collab;

static constexpr float kTick = 0.25f;        // seconds between sync steps
static constexpr float kFullCheck = 3.f;     // seconds between full checks
static constexpr double kFlashTime = 1.2;    // seconds a "just changed" box stays visible

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static bool blockingPopupOpen(bool countChat) {
    auto scene = CCDirector::get()->getRunningScene();
    if (!scene || !scene->getChildren()) return false;
    for (auto child : CCArrayExt<CCNode*>(scene->getChildren())) {
        bool isPopup = typeinfo_cast<FLAlertLayer*>(child) != nullptr || typeinfo_cast<EditorPauseLayer*>(child) != nullptr;
        if (!isPopup) continue;
        if (!countChat && child->getID() == "myriam.live-collab/live-collab-chat") continue;
        return true;
    }
    return false;
}

static void drawBox(CCDrawNode* node, CCRect const& r, ccColor4F border, float fillAlpha, float width) {
    CCPoint verts[4] = {
        { r.getMinX(), r.getMinY() }, { r.getMaxX(), r.getMinY() },
        { r.getMaxX(), r.getMaxY() }, { r.getMinX(), r.getMaxY() },
    };
    ccColor4F fill = border;
    fill.a = fillAlpha;
    node->drawPolygon(verts, 4, fill, width, border);
}

static CCRect unionOf(std::vector<CCRect> const& rects) {
    if (rects.empty()) return CCRect();
    float minX = rects[0].getMinX(), minY = rects[0].getMinY(), maxX = rects[0].getMaxX(), maxY = rects[0].getMaxY();
    for (auto const& r : rects) {
        minX = std::min(minX, r.getMinX()); minY = std::min(minY, r.getMinY());
        maxX = std::max(maxX, r.getMaxX()); maxY = std::max(maxY, r.getMaxY());
    }
    return CCRect(minX, minY, maxX - minX, maxY - minY);
}

// ---------------------------------------------------------------------------
// Count fingers on the editor (we wait while the player is touching)
// ---------------------------------------------------------------------------
class $modify(CollabEditorUI, EditorUI) {
    bool ccTouchBegan(CCTouch* touch, CCEvent* event) {
        bool claimed = EditorUI::ccTouchBegan(touch, event);
        if (claimed) G().touchesDown++;
        return claimed;
    }
    void ccTouchEnded(CCTouch* touch, CCEvent* event) {
        EditorUI::ccTouchEnded(touch, event);
        if (G().touchesDown > 0) G().touchesDown--;
    }
    void ccTouchCancelled(CCTouch* touch, CCEvent* event) {
        EditorUI::ccTouchCancelled(touch, event);
        if (G().touchesDown > 0) G().touchesDown--;
    }
};

// ---------------------------------------------------------------------------
// Movable chat button: tap = open chat, drag = move it (position is saved)
// ---------------------------------------------------------------------------
class ChatBubble : public CCNode, public CCTargetedTouchDelegate {
protected:
    CCNode* m_sprite = nullptr;
    CCLabelBMFont* m_badge = nullptr;
    CCPoint m_touchStart;
    CCPoint m_nodeStart;
    bool m_dragging = false;

    bool init() {
        if (!CCNode::init()) return false;
        auto label = CCLabelBMFont::create("Chat", "bigFont.fnt");
        auto spr = CircleButtonSprite::create(label, CircleBaseColor::Green, CircleBaseSize::Small);
        spr->setScale(.8f);
        m_sprite = spr;
        this->addChild(spr);

        m_badge = CCLabelBMFont::create("!", "bigFont.fnt");
        m_badge->setColor({ 255, 70, 70 });
        m_badge->setScale(.55f);
        m_badge->setPosition({ 16.f, 14.f });
        m_badge->setVisible(false);
        this->addChild(m_badge, 2);
        return true;
    }

public:
    static ChatBubble* create() {
        auto ret = new ChatBubble();
        if (ret->init()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }

    void setUnread(int count) {
        m_badge->setVisible(count > 0);
        if (count > 0) m_badge->setString(count > 9 ? "9+" : std::to_string(count).c_str());
    }

    void onEnter() override {
        CCNode::onEnter();
        CCTouchDispatcher::get()->addTargetedDelegate(this, -510, true);
    }

    void onExit() override {
        CCTouchDispatcher::get()->removeDelegate(this);
        CCNode::onExit();
    }

    bool ccTouchBegan(CCTouch* touch, CCEvent*) override {
        if (!this->isVisible() || !this->getParent()) return false;
        if (blockingPopupOpen(true)) return false;
        auto p = this->getParent()->convertToNodeSpace(touch->getLocation());
        if (ccpDistance(p, this->getPosition()) > 22.f) return false;
        m_touchStart = p;
        m_nodeStart = this->getPosition();
        m_dragging = false;
        m_sprite->setScale(.9f);
        return true;
    }

    void ccTouchMoved(CCTouch* touch, CCEvent*) override {
        auto p = this->getParent()->convertToNodeSpace(touch->getLocation());
        if (!m_dragging && ccpDistance(p, m_touchStart) > 8.f) m_dragging = true;
        if (!m_dragging) return;
        auto win = CCDirector::get()->getWinSize();
        auto pos = m_nodeStart + (p - m_touchStart);
        pos.x = std::clamp(pos.x, 20.f, win.width - 20.f);
        pos.y = std::clamp(pos.y, 20.f, win.height - 20.f);
        this->setPosition(pos);
    }

    void ccTouchEnded(CCTouch*, CCEvent*) override {
        m_sprite->setScale(.8f);
        if (m_dragging) {
            auto win = CCDirector::get()->getWinSize();
            Mod::get()->setSavedValue<float>("chat-x", this->getPositionX() / win.width);
            Mod::get()->setSavedValue<float>("chat-y", this->getPositionY() / win.height);
        } else {
            openChat();
        }
        m_dragging = false;
    }

    void ccTouchCancelled(CCTouch*, CCEvent*) override {
        m_sprite->setScale(.8f);
        m_dragging = false;
    }
};

// ---------------------------------------------------------------------------
// The editor itself
// ---------------------------------------------------------------------------
struct ObjEntry {
    std::string str;          // the object's save string
    float x = 0, y = 0, rot = 0, sx = 1, sy = 1;
    int id = 0;
    uint32_t gen = 0;
};

class $modify(CollabEditor, LevelEditorLayer) {
    struct Fields {
        std::unordered_map<GameObject*, ObjEntry> cache;
        uint32_t gen = 0;
        float sinceFull = 0.f;
        float sinceRequest = 0.f;
        float sinceSelection = 0.f;
        bool wasConnected = false;
        bool gotLevel = false;
        double touchSince = 0;
        std::string lastSelection;
        CCLabelBMFont* status = nullptr;
        CCDrawNode* overlay = nullptr;
        ChatBubble* bubble = nullptr;
        std::map<std::string, CCLabelBMFont*> nameLabels;
    };

    bool init(GJGameLevel* level, bool noUI) {
        if (!LevelEditorLayer::init(level, noUI)) return false;
        ensurePump();

        auto& g = G();
        auto f = m_fields.self();
        g.touchesDown = 0;
        g.editorQueue.clear();
        g.flashes.clear();
        f->gotLevel = g.freshLevel;     // guest: we just opened the host's level
        g.freshLevel = false;

        auto win = CCDirector::get()->getWinSize();
        auto status = CCLabelBMFont::create("", "bigFont.fnt");
        status->setScale(.28f);
        status->setOpacity(200);
        status->setPosition({ win.width / 2.f, win.height - 8.f });
        status->setID("status-label"_spr);
        this->addChild(status, 1000);
        f->status = status;

        if (m_objectLayer) {
            auto overlay = CCDrawNode::create();
            overlay->setID("friend-boxes"_spr);
            m_objectLayer->addChild(overlay, 9999);
            f->overlay = overlay;
        }

        if (auto bubble = ChatBubble::create()) {
            float rx = Mod::get()->getSavedValue<float>("chat-x", .62f);
            float ry = Mod::get()->getSavedValue<float>("chat-y", .9f);
            bubble->setPosition({ std::clamp(rx, 0.f, 1.f) * win.width, std::clamp(ry, 0.f, 1.f) * win.height });
            bubble->setID("chat-button"_spr);
            bubble->setVisible(false);
            this->addChild(bubble, 1001);
            f->bubble = bubble;
        }

        this->collabRebuildCache();
        this->schedule(schedule_selector(CollabEditor::collabTick), kTick);
        return true;
    }

    // ----- local state -------------------------------------------------------
    ObjEntry collabEntryFor(GameObject* obj) {
        ObjEntry e;
        e.str = std::string(obj->getSaveString(this));
        e.x = obj->getPositionX();
        e.y = obj->getPositionY();
        e.rot = obj->getRotation();
        e.sx = obj->getScaleX();
        e.sy = obj->getScaleY();
        e.id = obj->m_objectID;
        e.gen = m_fields->gen;
        return e;
    }

    void collabRebuildCache() {
        auto f = m_fields.self();
        f->cache.clear();
        f->gen++;
        if (!m_objects) return;
        f->cache.reserve(m_objects->count());
        for (auto obj : CCArrayExt<GameObject*>(m_objects)) {
            if (obj) f->cache.emplace(obj, this->collabEntryFor(obj));
        }
    }

    // Finds what *I* changed since the last check
    void collabScan(collab::Diff& diff, bool full) {
        auto f = m_fields.self();
        if (!m_objects) return;

        std::unordered_set<GameObject*> selected;
        if (m_editorUI) {
            if (auto arr = m_editorUI->getSelectedObjects()) {
                for (auto obj : CCArrayExt<GameObject*>(arr)) selected.insert(obj);
            }
        }

        uint32_t gen = ++f->gen;
        for (auto obj : CCArrayExt<GameObject*>(m_objects)) {
            if (!obj) continue;
            auto it = f->cache.find(obj);
            if (it == f->cache.end()) {
                auto e = this->collabEntryFor(obj);
                diff.added.push_back(e.str);
                f->cache.emplace(obj, std::move(e));
                continue;
            }
            auto& e = it->second;
            e.gen = gen;
            bool moved = e.x != obj->getPositionX() || e.y != obj->getPositionY() ||
                e.rot != obj->getRotation() || e.sx != obj->getScaleX() || e.sy != obj->getScaleY() ||
                e.id != obj->m_objectID;
            if (!(full || moved || selected.count(obj))) continue;
            std::string str = obj->getSaveString(this);
            if (str != e.str) {
                diff.removed.push_back(e.str);
                diff.added.push_back(str);
                e.str = std::move(str);
            }
            e.x = obj->getPositionX(); e.y = obj->getPositionY();
            e.rot = obj->getRotation(); e.sx = obj->getScaleX(); e.sy = obj->getScaleY();
            e.id = obj->m_objectID;
        }
        for (auto it = f->cache.begin(); it != f->cache.end();) {
            if (it->second.gen != gen) {
                diff.removed.push_back(it->second.str);
                it = f->cache.erase(it);
            } else {
                ++it;
            }
        }
    }

    void collabSendSelection(float dt) {
        auto f = m_fields.self();
        std::vector<CCRect> rects;
        if (m_editorUI) {
            if (auto arr = m_editorUI->getSelectedObjects()) {
                for (auto obj : CCArrayExt<GameObject*>(arr)) rects.push_back(obj->getObjectRect());
            }
        }
        if (rects.size() > 40) rects = { unionOf(rects) };
        std::string body;
        char buf[96];
        for (auto const& r : rects) {
            snprintf(buf, sizeof(buf), "%.0f,%.0f,%.0f,%.0f;", r.origin.x, r.origin.y, r.size.width, r.size.height);
            body += buf;
        }
        f->sinceSelection += dt;
        if (body != f->lastSelection || f->sinceSelection > 3.f) {
            f->lastSelection = body;
            f->sinceSelection = 0.f;
            Session::get().send('S', myName() + "\n" + body);
        }
    }

    void collabSendFull() {
        std::string level = this->getLevelString();
        FullLevel full;
        full.compressed = std::string(ZipUtils::compressString(level, false, 0));
        if (m_level) {
            full.audioTrack = m_level->m_audioTrack;
            full.songID = m_level->m_songID;
            full.songIDs = std::string(m_level->m_songIDs);
            full.sfxIDs = std::string(m_level->m_sfxIDs);
        }
        Session::get().send('F', encodeFull(full));
    }

    // ----- applying my friend's changes --------------------------------------
    void collabFlash(std::string const& sender, std::vector<CCRect> rects, bool removed) {
        if (rects.empty()) return;
        if (rects.size() > 40) rects = { unionOf(rects) };
        double until = now() + kFlashTime;
        auto& flashes = G().flashes;
        for (auto const& r : rects) flashes.push_back({ sender, r, until, removed });
        if (flashes.size() > 300) flashes.erase(flashes.begin(), flashes.begin() + (flashes.size() - 300));
    }

    void collabApplyDiff(std::string const& body, std::string const& sender) {
        auto f = m_fields.self();
        auto diff = collab::decodeDiff(body);
        if (diff.empty()) return;

        if (!diff.removed.empty()) {
            std::unordered_map<std::string, std::vector<GameObject*>> exact, loose;
            bool looseReady = false;
            for (auto const& [obj, e] : f->cache) exact[e.str].push_back(obj);

            std::unordered_set<GameObject*> chosen;
            std::vector<GameObject*> toRemove;
            auto take = [&](std::vector<GameObject*>& list) -> GameObject* {
                while (!list.empty()) {
                    auto obj = list.back();
                    list.pop_back();
                    if (!chosen.count(obj)) return obj;
                }
                return nullptr;
            };
            for (auto const& str : diff.removed) {
                GameObject* obj = nullptr;
                if (auto it = exact.find(str); it != exact.end()) obj = take(it->second);
                if (!obj) {
                    if (!looseReady) {
                        for (auto const& [o, e] : f->cache) loose[collab::signatureOf(e.str)].push_back(o);
                        looseReady = true;
                    }
                    if (auto it = loose.find(collab::signatureOf(str)); it != loose.end()) obj = take(it->second);
                }
                if (obj) { chosen.insert(obj); toRemove.push_back(obj); }
            }

            bool anySelected = false;
            std::vector<CCRect> rects;
            for (auto obj : toRemove) {
                if (obj->m_isSelected) anySelected = true;
                rects.push_back(obj->getObjectRect());
            }
            if (anySelected && m_editorUI) m_editorUI->deselectAll();
            for (auto obj : toRemove) {
                f->cache.erase(obj);
                this->removeObject(obj, true);
            }
            this->collabFlash(sender, std::move(rects), true);
        }

        if (!diff.added.empty()) {
            std::string joined;
            for (auto const& s : diff.added) { joined += s; joined += ';'; }
            std::vector<CCRect> rects;
            if (auto arr = this->createObjectsFromString(joined, true, true)) {
                for (auto obj : CCArrayExt<GameObject*>(arr)) {
                    if (!obj) continue;
                    f->cache[obj] = this->collabEntryFor(obj);
                    rects.push_back(obj->getObjectRect());
                }
            }
            this->collabFlash(sender, std::move(rects), false);
        }
    }

    void collabApplyFull(std::string const& body) {
        FullLevel full;
        if (!decodeFull(body, full)) return;
        std::string level = std::string(ZipUtils::decompressString(full.compressed, false, 0));
        auto semi = level.find(';');
        std::string objects = semi == std::string::npos ? std::string() : level.substr(semi + 1);

        if (m_editorUI) m_editorUI->deselectAll();
        std::vector<GameObject*> all;
        if (m_objects) for (auto obj : CCArrayExt<GameObject*>(m_objects)) all.push_back(obj);
        for (auto obj : all) this->removeObject(obj, true);
        if (!objects.empty()) this->createObjectsFromString(objects, true, true);

        if (m_level) {
            m_level->m_audioTrack = full.audioTrack;
            m_level->m_songID = full.songID;
            m_level->m_songIDs = full.songIDs;
            m_level->m_sfxIDs = full.sfxIDs;
        }
        this->collabRebuildCache();
        m_fields->gotLevel = true;
        Notification::create("Level received from the host!", NotificationIcon::Success, 2.f)->show();
    }

    // ----- drawing the friend's boxes ----------------------------------------
    void collabDrawOverlay() {
        auto f = m_fields.self();
        if (!f->overlay) return;
        f->overlay->clear();
        auto& g = G();
        float zoom = std::max(.1f, m_objectLayer ? m_objectLayer->getScale() : 1.f);
        float width = 1.2f / zoom;
        double t = now();

        // Where each player's name goes
        std::map<std::string, CCRect> anchor;

        auto& flashes = g.flashes;
        flashes.erase(std::remove_if(flashes.begin(), flashes.end(), [&](Flash const& fl) { return fl.until < t; }), flashes.end());
        for (auto const& fl : flashes) {
            auto c = colorFor(fl.sender);
            c.a = static_cast<float>(std::clamp((fl.until - t) / kFlashTime, 0.0, 1.0));
            drawBox(f->overlay, fl.rect, c, fl.removed ? .05f * c.a : .25f * c.a, width);
            anchor[fl.sender] = fl.rect;
        }
        for (auto const& [id, p] : g.players) {
            if (p.selection.empty()) continue;
            auto c = colorFor(id);
            for (auto const& r : p.selection) drawBox(f->overlay, r, c, .12f, width);
            anchor[id] = unionOf(p.selection);
        }

        // Name tags
        for (auto it = f->nameLabels.begin(); it != f->nameLabels.end();) {
            if (!anchor.count(it->first)) {
                it->second->removeFromParent();
                it = f->nameLabels.erase(it);
            } else {
                ++it;
            }
        }
        for (auto const& [id, rect] : anchor) {
            auto& label = f->nameLabels[id];
            if (!label) {
                label = CCLabelBMFont::create("", "bigFont.fnt");
                label->setAnchorPoint({ 0.f, 0.f });
                f->overlay->addChild(label);
            }
            auto name = nameOf(id);
            if (name != label->getString()) label->setString(name.c_str());
            auto c = colorFor(id);
            label->setColor({ static_cast<GLubyte>(c.r * 255), static_cast<GLubyte>(c.g * 255), static_cast<GLubyte>(c.b * 255) });
            label->setScale(.35f / zoom);
            label->setPosition({ rect.getMinX(), rect.getMaxY() + 2.f / zoom });
        }
    }

    // ----- main loop -----------------------------------------------------------
    void collabTick(float dt) {
        auto& s = Session::get();
        auto& g = G();
        auto f = m_fields.self();
        bool connected = s.state() == State::Connected;

        if (f->bubble) {
            f->bubble->setVisible(s.active());
            f->bubble->setUnread(g.unread);
        }
        if (f->status) {
            std::string text;
            if (s.active() && !connected) text = fmt::format("Live Collab: connecting to room {}...", s.room());
            else if (connected && !s.isHost() && !f->gotLevel) text = fmt::format("Live Collab: room {} - waiting for the host's level...", s.room());
            else if (connected) text = fmt::format("Live Collab: room {} ({})", s.room(), s.isHost() ? "host" : "guest");
            f->status->setString(text.c_str());
        }

        if (!connected) {
            f->wasConnected = false;
            if (f->overlay) f->overlay->clear();
            for (auto& [id, label] : f->nameLabels) label->removeFromParent();
            f->nameLabels.clear();
            return;
        }
        // Nothing is synced while play-testing (objects move during the test)
        if (m_playbackMode != PlaybackMode::Not) return;

        // Safety: never wait forever on a touch we missed
        if (g.touchesDown > 0) {
            if (f->touchSince == 0) f->touchSince = now();
            else if (now() - f->touchSince > 6.0) g.touchesDown = 0;
        } else {
            f->touchSince = 0;
        }

        // 1) what I changed
        f->sinceFull += dt;
        bool full = f->sinceFull >= kFullCheck;
        if (full) f->sinceFull = 0.f;
        collab::Diff diff;
        this->collabScan(diff, full);
        if (!diff.empty()) s.send('D', collab::encodeDiff(diff));

        // Just connected (or entered the editor while connected)
        if (!f->wasConnected) {
            f->wasConnected = true;
            f->sinceRequest = 0.f;
            if (s.isHost()) this->collabSendFull();
            else if (!f->gotLevel) s.send('R', "");
        }
        if (!s.isHost() && !f->gotLevel) {
            f->sinceRequest += dt;
            if (f->sinceRequest > 4.f) { f->sinceRequest = 0.f; s.send('R', ""); }
        }

        this->collabSendSelection(dt);

        // 2) my friend's changes - but not while I'm touching / editing an object
        bool busy = g.touchesDown > 0 || blockingPopupOpen(false);
        if (!busy) {
            bool sentFull = false;
            while (!g.editorQueue.empty()) {
                Message msg = std::move(g.editorQueue.front());
                g.editorQueue.pop_front();
                switch (msg.type) {
                    case 'D':
                        this->collabApplyDiff(msg.body, msg.sender);
                        break;
                    case 'F':
                        if (!s.isHost()) this->collabApplyFull(msg.body);
                        break;
                    case 'R':
                    case 'J':
                        if (s.isHost() && !sentFull) { this->collabSendFull(); sentFull = true; }
                        break;
                    default:
                        break;
                }
            }
        }

        this->collabDrawOverlay();
    }
};
