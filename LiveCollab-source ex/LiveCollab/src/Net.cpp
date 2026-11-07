#include "Net.hpp"

#include <cstring>
#include <vector>

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    using sock_t = SOCKET;
    #define CLOSESOCK closesocket
    static bool sockWouldBlock() {
        int e = WSAGetLastError();
        return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS;
    }
#else
    #include <sys/types.h>
    #include <sys/socket.h>
    #include <sys/select.h>
    #include <netdb.h>
    #include <netinet/in.h>
    #include <netinet/tcp.h>
    #include <fcntl.h>
    #include <unistd.h>
    #include <errno.h>
    using sock_t = int;
    #define CLOSESOCK ::close
    static bool sockWouldBlock() { return errno == EINPROGRESS || errno == EWOULDBLOCK; }
#endif

namespace collab {

static void netInitOnce() {
#ifdef _WIN32
    static bool done = false;
    if (!done) {
        WSADATA d;
        WSAStartup(MAKEWORD(2, 2), &d);
        done = true;
    }
#endif
}

static void setBlocking(sock_t s, bool blocking) {
#ifdef _WIN32
    u_long mode = blocking ? 0 : 1;
    ioctlsocket(s, FIONBIO, &mode);
#else
    int flags = fcntl(s, F_GETFL, 0);
    if (blocking) flags &= ~O_NONBLOCK; else flags |= O_NONBLOCK;
    fcntl(s, F_SETFL, flags);
#endif
}

// ---------- MQTT encoding helpers ----------
static std::string encLen(size_t len) {
    std::string out;
    do {
        uint8_t b = len % 128;
        len /= 128;
        if (len > 0) b |= 0x80;
        out.push_back(static_cast<char>(b));
    } while (len > 0);
    return out;
}

static std::string encStr(std::string const& s) {
    std::string out;
    out.push_back(static_cast<char>((s.size() >> 8) & 0xFF));
    out.push_back(static_cast<char>(s.size() & 0xFF));
    out += s;
    return out;
}

static std::string packet(uint8_t header, std::string const& body) {
    std::string out;
    out.push_back(static_cast<char>(header));
    out += encLen(body.size());
    out += body;
    return out;
}

MqttClient::~MqttClient() { close(); }

std::string MqttClient::connect(std::string const& host, int port, std::string const& clientId, int timeoutSec) {
    netInitOnce();
    close();

    addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    std::string portStr = std::to_string(port);
    if (getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res) != 0 || !res) {
        return "Could not find server " + host + " (no internet?)";
    }

    sock_t s = (sock_t)-1;
    bool ok = false;
    for (addrinfo* ai = res; ai && !ok; ai = ai->ai_next) {
        s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
#ifdef _WIN32
        if (s == INVALID_SOCKET) continue;
#else
        if (s < 0) continue;
#endif
        setBlocking(s, false);
        int r = ::connect(s, ai->ai_addr, (int)ai->ai_addrlen);
        if (r == 0) {
            ok = true;
        } else if (sockWouldBlock()) {
            fd_set wf; FD_ZERO(&wf); FD_SET(s, &wf);
            timeval tv { timeoutSec, 0 };
            if (select((int)s + 1, nullptr, &wf, nullptr, &tv) > 0) {
                int err = 0;
                socklen_t len = sizeof(err);
                getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&err, &len);
                ok = (err == 0);
            }
        }
        if (!ok) { CLOSESOCK(s); s = (sock_t)-1; }
    }
    freeaddrinfo(res);
    if (!ok) return "Could not reach server " + host;

    setBlocking(s, true);
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char const*)&one, sizeof(one));
#ifdef SO_NOSIGPIPE
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, (char const*)&one, sizeof(one));
#endif
    {
        std::lock_guard lock(m_writeMutex);
        m_sock = (intptr_t)s;
        m_open = true;
    }

    // CONNECT
    std::string body;
    body += encStr("MQTT");
    body.push_back(4);          // protocol level 3.1.1
    body.push_back(0x02);       // clean session
    body.push_back(0);          // keep alive = 60 s
    body.push_back(60);
    body += encStr(clientId);
    if (!sendAll(packet(0x10, body))) { close(); return "Send failed"; }

    uint8_t type = 0;
    std::string ack;
    int r = readPacket(timeoutSec * 1000, type, ack);
    if (r <= 0 || (type >> 4) != 2 || ack.size() < 2) { close(); return "Server did not answer"; }
    if (ack[1] != 0) { close(); return "Server refused connection (code " + std::to_string((int)ack[1]) + ")"; }
    return "";
}

bool MqttClient::subscribe(std::string const& topic) {
    std::string body;
    uint16_t id = m_packetId++;
    body.push_back(static_cast<char>(id >> 8));
    body.push_back(static_cast<char>(id & 0xFF));
    body += encStr(topic);
    body.push_back(0); // QoS 0
    return sendAll(packet(0x82, body));
}

bool MqttClient::publish(std::string const& topic, std::string const& payload) {
    std::string body = encStr(topic);
    body += payload;
    return sendAll(packet(0x30, body));
}

bool MqttClient::ping() {
    return sendAll(std::string("\xC0\x00", 2));
}

void MqttClient::close() {
    std::lock_guard lock(m_writeMutex);
    if (m_sock != -1) {
        if (m_open) {
            char d[2] = { (char)0xE0, 0 };
            ::send((sock_t)m_sock, d, 2, 0);
        }
        CLOSESOCK((sock_t)m_sock);
        m_sock = -1;
    }
    m_open = false;
}

bool MqttClient::sendAll(std::string const& data) {
    std::lock_guard lock(m_writeMutex);
    if (m_sock == -1 || !m_open) return false;
    size_t sent = 0;
    while (sent < data.size()) {
#ifdef MSG_NOSIGNAL
        int flags = MSG_NOSIGNAL;
#else
        int flags = 0;
#endif
        int n = ::send((sock_t)m_sock, data.data() + sent, (int)(data.size() - sent), flags);
        if (n <= 0) { m_open = false; return false; }
        sent += (size_t)n;
    }
    return true;
}

bool MqttClient::recvExact(char* buf, size_t n, int timeoutMs) {
    size_t got = 0;
    while (got < n) {
        if (m_sock == -1) return false;
        fd_set rf; FD_ZERO(&rf); FD_SET((sock_t)m_sock, &rf);
        timeval tv { timeoutMs / 1000, (timeoutMs % 1000) * 1000 };
        int sel = select((int)m_sock + 1, &rf, nullptr, nullptr, &tv);
        if (sel <= 0) return false;
        int r = ::recv((sock_t)m_sock, buf + got, (int)(n - got), 0);
        if (r <= 0) { m_open = false; return false; }
        got += (size_t)r;
    }
    return true;
}

// 1 = packet read, 0 = timeout before any byte, -1 = error
int MqttClient::readPacket(int timeoutMs, uint8_t& type, std::string& body) {
    if (m_sock == -1 || !m_open) return -1;
    fd_set rf; FD_ZERO(&rf); FD_SET((sock_t)m_sock, &rf);
    timeval tv { timeoutMs / 1000, (timeoutMs % 1000) * 1000 };
    int sel = select((int)m_sock + 1, &rf, nullptr, nullptr, &tv);
    if (sel == 0) return 0;
    if (sel < 0) { m_open = false; return -1; }

    char h;
    if (!recvExact(&h, 1, 10000)) { m_open = false; return -1; }
    type = static_cast<uint8_t>(h);

    size_t len = 0, mult = 1;
    for (int i = 0; i < 4; i++) {
        char b;
        if (!recvExact(&b, 1, 10000)) { m_open = false; return -1; }
        len += (static_cast<uint8_t>(b) & 0x7F) * mult;
        mult *= 128;
        if (!(static_cast<uint8_t>(b) & 0x80)) break;
    }
    body.assign(len, '\0');
    if (len > 0 && !recvExact(body.data(), len, 15000)) { m_open = false; return -1; }
    return 1;
}

int MqttClient::poll(int timeoutMs, std::string& topic, std::string& payload) {
    uint8_t type = 0;
    std::string body;
    int r = readPacket(timeoutMs, type, body);
    if (r < 0) return -1;
    if (r == 0) return 0;
    if ((type >> 4) == 3) { // PUBLISH
        if (body.size() < 2) return 0;
        size_t tlen = (static_cast<uint8_t>(body[0]) << 8) | static_cast<uint8_t>(body[1]);
        if (body.size() < 2 + tlen) return 0;
        topic = body.substr(2, tlen);
        size_t off = 2 + tlen;
        int qos = (type >> 1) & 3;
        if (qos > 0) off += 2; // skip packet id (we only subscribe at QoS 0 anyway)
        payload = off <= body.size() ? body.substr(off) : std::string();
        return 1;
    }
    return 0; // SUBACK, PINGRESP, ...
}

} // namespace collab
