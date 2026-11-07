#pragma once
// Room / connection manager. No Geode dependency (testable on its own).
//
// Both players connect to a free public relay server (MQTT broker) and talk
// on a topic named after the room code. No port forwarding, no IP addresses:
// the host gets a 6-letter code, the friend types it, done.

#include "Net.hpp"
#include <string>
#include <vector>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <atomic>

namespace collab {

enum class State { Off, Connecting, Connected, Failed };

// Message types exchanged between players
//  'J' joiner says hello (name)     'W' host answers "welcome" (name)
//  'C' chat (name\ntext)            'S' selected objects (name\nrects)
//  'R' "please send me the level"   'F' full level (compressed)
//  'D' object changes (diff)        'B' player left
//  'P' piece of a big message (internal, reassembled automatically)
struct Message {
    std::string sender;
    char type;
    std::string body;
};

class Session {
public:
    static Session& get();

    void host();                         // creates a new room code and connects
    void join(std::string const& code);  // connects to an existing room
    void leave();

    State state() const { return m_state.load(); }
    bool active() const { auto s = m_state.load(); return s == State::Connecting || s == State::Connected; }
    bool isHost() const { return m_isHost; }
    std::string room();
    std::string myId();
    std::string server();
    std::string lastError();

    // The name shown to the other players (GD username)
    void setName(std::string const& name);
    std::string name();

    // Sends a message to everyone else in the room (large messages are split automatically).
    void send(char type, std::string const& body);

    // Messages received from the other player(s), oldest first.
    std::vector<Message> drain();
    void clearInbox();

    // Short human-readable events for pop-up notifications ("Friend joined!" ...).
    std::vector<std::string> drainNotices();

    static std::string normalizeCode(std::string const& code);
    static std::string makeCode();

    // Relay servers tried in order (free public MQTT brokers).
    static std::vector<std::pair<std::string, int>> const& servers();

private:
    Session() = default;
    void start(std::string const& room, bool isHost);
    void run(std::string room, bool isHost, uint64_t generation);
    void handleRaw(std::string const& payload);
    void deliver(Message msg);
    void notice(std::string const& text);
    bool current(uint64_t gen) const { return m_generation.load() == gen; }

    std::atomic<State> m_state { State::Off };
    std::atomic<uint64_t> m_generation { 0 };
    std::atomic<bool> m_isHost { false };

    std::mutex m_mutex;              // guards everything below
    std::shared_ptr<MqttClient> m_client;
    std::string m_room;
    std::string m_myId;
    std::string m_topic;
    std::string m_server;
    std::string m_name = "Player";
    uint32_t m_nextMsgId = 1;
    std::deque<Message> m_inbox;
    std::vector<std::string> m_notices;
    std::string m_lastError;
    struct Partial { size_t count = 0; size_t received = 0; char type = 0; std::vector<std::string> parts; };
    std::map<std::string, Partial> m_partials;
};

} // namespace collab
