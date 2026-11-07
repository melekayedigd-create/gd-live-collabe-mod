#include "Session.hpp"

   #include <cstdio>
   #include <cstdlib>
   #include <iterator>

#include <thread>
#include <chrono>
#include <random>
#include <cctype>

namespace collab {

static constexpr char const* kMagic = "GDC1";
static constexpr size_t kChunk = 24000;     // bytes per relay message (public servers limit size)
static constexpr size_t kMaxInbox = 20000;

Session& Session::get() {
    static Session s;
    return s;
}

std::vector<std::pair<std::string, int>> const& Session::servers() {
    static std::vector<std::pair<std::string, int>> list = {
#ifdef COLLAB_TEST_SERVER
        { COLLAB_TEST_SERVER, 1883 },
#endif
        { "broker.hivemq.com", 1883 },
        { "broker.emqx.io", 1883 },
        { "test.mosquitto.org", 1883 },
    };
    return list;
}

static std::mt19937& rng() {
    static std::mt19937 r { std::random_device{}() ^ (unsigned)std::chrono::steady_clock::now().time_since_epoch().count() };
    return r;
}

std::string Session::makeCode() {
    // No 0/O/1/I so codes are easy to read and type
    static char const* alphabet = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    std::uniform_int_distribution<int> d(0, 31);
    std::string code;
    for (int i = 0; i < 6; i++) code.push_back(alphabet[d(rng())]);
    return code;
}

std::string Session::normalizeCode(std::string const& code) {
    std::string out;
    for (char c : code) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        }
    }
    return out;
}

std::string Session::room() { std::lock_guard l(m_mutex); return m_room; }
std::string Session::myId() { std::lock_guard l(m_mutex); return m_myId; }
std::string Session::server() { std::lock_guard l(m_mutex); return m_server; }
std::string Session::lastError() { std::lock_guard l(m_mutex); return m_lastError; }

void Session::host() { start(makeCode(), true); }

void Session::join(std::string const& code) { start(normalizeCode(code), false); }

void Session::start(std::string const& room, bool isHost) {
    leave();
    std::uniform_int_distribution<uint32_t> d;
    {
        std::lock_guard l(m_mutex);
        m_room = room;
        m_topic = std::string("gdcollab/v1/") + room;
        char buf[16];
        snprintf(buf, sizeof(buf), "%08x", d(rng()));
        m_myId = buf;
        m_inbox.clear();
        m_partials.clear();
        m_lastError.clear();
        m_server.clear();
    }
    m_isHost = isHost;
    m_state = State::Connecting;
    uint64_t gen = ++m_generation;
    std::thread([this, room, isHost, gen] { run(room, isHost, gen); }).detach();
}

void Session::leave() {
    if (active()) send('B', "");
    ++m_generation;             // tells the network thread to stop
    std::shared_ptr<MqttClient> old;
    {
        std::lock_guard l(m_mutex);
        old = std::move(m_client);
        m_inbox.clear();
        m_partials.clear();
    }
    m_state = State::Off;
    // `old` is closed by its own thread (it holds a reference too)
}

void Session::notice(std::string const& text) {
    std::lock_guard l(m_mutex);
    m_notices.push_back(text);
}

std::vector<std::string> Session::drainNotices() {
    std::lock_guard l(m_mutex);
    auto out = std::move(m_notices);
    m_notices.clear();
    return out;
}

std::vector<Message> Session::drain() {
    std::lock_guard l(m_mutex);
    std::vector<Message> out(std::make_move_iterator(m_inbox.begin()), std::make_move_iterator(m_inbox.end()));
    m_inbox.clear();
    return out;
}

void Session::clearInbox() {
    std::lock_guard l(m_mutex);
    m_inbox.clear();
}

void Session::send(char type, std::string const& body) {
    std::shared_ptr<MqttClient> client;
    std::string topic, me;
    uint32_t msgId;
    {
        std::lock_guard l(m_mutex);
        client = m_client;
        topic = m_topic;
        me = m_myId;
        msgId = m_nextMsgId++;
    }
    if (!client || !client->isOpen()) return;

    auto wrap = [&](char t, std::string const& b) {
        std::string p = kMagic;
        p += '\n'; p += me; p += '\n'; p += t; p += '\n'; p += b;
        return p;
    };

    if (body.size() <= kChunk) {
        client->publish(topic, wrap(type, body));
        return;
    }
    size_t count = (body.size() + kChunk - 1) / kChunk;
    for (size_t i = 0; i < count; i++) {
        std::string piece = std::to_string(msgId) + "|" + std::to_string(i) + "|" + std::to_string(count) + "|" + type + "|";
        piece += body.substr(i * kChunk, kChunk);
        if (!client->publish(topic, wrap('P', piece))) return;
    }
}

void Session::deliver(Message msg) {
    bool isHost = m_isHost;
    if (msg.type == 'J') {
        notice(isHost ? "Your friend joined the room!" : "Another player joined the room");
        if (isHost) send('W', "");
    }
    else if (msg.type == 'W' && !isHost) {
        notice("Connected to your friend's room!");
    }
    else if (msg.type == 'B') {
        notice(msg.sender.empty() ? "A player left" : "Your friend left the room");
    }
    std::lock_guard l(m_mutex);
    if (m_inbox.size() < kMaxInbox) m_inbox.push_back(std::move(msg));
}

void Session::handleRaw(std::string const& payload) {
    // GDC1\n<sender>\n<type>\n<body>
    size_t a = payload.find('\n');
    if (a == std::string::npos || payload.compare(0, a, kMagic) != 0) return;
    size_t b = payload.find('\n', a + 1);
    if (b == std::string::npos || b + 2 >= payload.size() + 1) return;
    std::string sender = payload.substr(a + 1, b - a - 1);
    if (b + 1 >= payload.size()) return;
    char type = payload[b + 1];
    std::string body = (b + 3 <= payload.size()) ? payload.substr(b + 3) : std::string();

    if (sender == myId()) return; // our own message echoed back by the server

    if (type != 'P') {
        deliver({ sender, type, std::move(body) });
        return;
    }

    // Piece of a big message: id|index|count|type|data
    size_t p1 = body.find('|');
    size_t p2 = body.find('|', p1 + 1);
    size_t p3 = body.find('|', p2 + 1);
    size_t p4 = body.find('|', p3 + 1);
    if (p1 == std::string::npos || p2 == std::string::npos || p3 == std::string::npos || p4 == std::string::npos) return;
    std::string key = sender + ":" + body.substr(0, p1);
    size_t index = std::strtoul(body.substr(p1 + 1, p2 - p1 - 1).c_str(), nullptr, 10);
    size_t count = std::strtoul(body.substr(p2 + 1, p3 - p2 - 1).c_str(), nullptr, 10);
    char origType = body[p3 + 1];
    if (count == 0 || count > 100000 || index >= count) return;

    Message done;
    bool complete = false;
    {
        std::lock_guard l(m_mutex);
        auto& part = m_partials[key];
        if (part.count == 0) { part.count = count; part.type = origType; part.parts.resize(count); }
        if (part.count != count) return;
        if (part.parts[index].empty()) {
            part.parts[index] = body.substr(p4 + 1);
            part.received++;
        }
        if (part.received == part.count) {
            done.sender = sender;
            done.type = part.type;
            for (auto& s : part.parts) done.body += s;
            m_partials.erase(key);
            complete = true;
        }
        if (m_partials.size() > 64) m_partials.clear(); // stale junk protection
    }
    if (complete) deliver(std::move(done));
}

void Session::run(std::string room, bool isHost, uint64_t gen) {
    std::string topic = std::string("gdcollab/v1/") + room;
    int failures = 0;
    bool everConnected = false;

    while (current(gen)) {
        auto client = std::make_shared<MqttClient>();
        std::string error;
        std::string usedServer;
        for (auto const& [hostName, port] : servers()) {
            if (!current(gen)) return;
            std::string id = "gdc-" + myId() + "-" + std::to_string(failures);
            error = client->connect(hostName, port, id);
            if (error.empty()) { usedServer = hostName; break; }
        }
        if (!current(gen)) { client->close(); return; }

        if (!error.empty() || !client->subscribe(topic)) {
            failures++;
            {
                std::lock_guard l(m_mutex);
                m_lastError = error.empty() ? "Could not join the room" : error;
            }
            if (!everConnected || failures >= 3) {
                m_state = State::Failed;
                notice("Connection failed: " + lastError());
                return;
            }
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }

        {
            std::lock_guard l(m_mutex);
            m_client = client;
            m_server = usedServer;
        }
        m_state = State::Connected;
        failures = 0;
        if (!everConnected) {
            notice(isHost ? "Room " + room + " is open!" : "Joined room " + room + ", waiting for your friend...");
        } else {
            notice("Reconnected");
        }
        everConnected = true;
        if (!isHost) send('J', "");

        auto lastPing = std::chrono::steady_clock::now();
        while (current(gen)) {
            std::string t, payload;
            int r = client->poll(400, t, payload);
            if (r < 0) break;
            if (r == 1 && t == topic) handleRaw(payload);
            auto now = std::chrono::steady_clock::now();
            if (now - lastPing > std::chrono::seconds(20)) {
                if (!client->ping()) break;
                lastPing = now;
            }
        }
        client->close();
        if (!current(gen)) return;

        // Lost the connection: try again
        m_state = State::Connecting;
        notice("Connection lost, reconnecting...");
        {
            std::lock_guard l(m_mutex);
            m_client.reset();
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

} // namespace collab
