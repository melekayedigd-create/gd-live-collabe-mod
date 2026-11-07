#pragma once
// Tiny MQTT 3.1.1 client (QoS 0 only) over a plain TCP socket.
// No Geode dependency, so it can be tested on its own.

#include <string>
#include <mutex>
#include <atomic>
#include <cstdint>

namespace collab {

class MqttClient {
public:
    MqttClient() = default;
    ~MqttClient();

    // Blocking. Returns "" on success, otherwise an error message.
    std::string connect(std::string const& host, int port, std::string const& clientId, int timeoutSec = 8);
    bool subscribe(std::string const& topic);
    bool publish(std::string const& topic, std::string const& payload);
    bool ping();
    void close();
    bool isOpen() const { return m_open.load(); }

    // Waits up to `timeoutMs` for one packet.
    // Returns 1 = got a PUBLISH (topic/payload filled), 0 = nothing / other packet, -1 = connection lost.
    int poll(int timeoutMs, std::string& topic, std::string& payload);

private:
    bool sendAll(std::string const& data);
    bool recvExact(char* buf, size_t n, int timeoutMs);
    int readPacket(int timeoutMs, uint8_t& type, std::string& body);

    intptr_t m_sock = -1;
    std::atomic<bool> m_open { false };
    std::mutex m_writeMutex;
    uint16_t m_packetId = 1;
};

} // namespace collab
