#pragma once
// Shared state between the menu, the editor and the chat.

#include <Geode/Geode.hpp>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "Session.hpp"

namespace collab {

struct ChatLine {
    std::string name;
    std::string text;
    bool mine = false;
};

// Boxes drawn around what the other players are doing
struct Flash {
    std::string sender;
    cocos2d::CCRect rect;
    double until = 0;
    bool removed = false;
};

struct RemotePlayer {
    std::string name = "Friend";
    std::vector<cocos2d::CCRect> selection;
    double lastSeen = 0;
};

// What the host sends: the whole level + its song
struct FullLevel {
    int audioTrack = 0;
    int songID = 0;
    std::string songIDs;
    std::string sfxIDs;
    std::string compressed;   // GD's own compressed level string (settings, colors, objects)
};

struct Globals {
    std::vector<ChatLine> chat;
    int unread = 0;
    bool chatOpen = false;

    std::map<std::string, RemotePlayer> players;   // by sender id
    std::vector<Flash> flashes;

    std::deque<Message> editorQueue;               // D / F / R / J messages for the editor

    geode::Ref<GJGameLevel> collabLevel;           // guest: the level created for this room
    std::string collabRoom;
    bool freshLevel = false;                       // guest: editor opened with a just-received level
    bool hasPendingFull = false;                   // guest: level arrived while playing a level
    FullLevel pendingFull;

    geode::Ref<FLAlertLayer> waitingPopup;         // guest: "waiting for host" popup
    bool closingWaiting = false;

    int touchesDown = 0;                           // fingers currently touching the editor
};

Globals& G();
double now();
std::string myName();
cocos2d::ccColor4F colorFor(std::string const& sender);
std::string nameOf(std::string const& sender);

void ensurePump();
void openChat();
bool chatIsShowing();
void addChatLine(std::string const& name, std::string const& text, bool mine);

std::string encodeFull(FullLevel const& level);
bool decodeFull(std::string const& body, FullLevel& out);

} // namespace collab
