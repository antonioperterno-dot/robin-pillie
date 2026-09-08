#pragma once

#include <cstdint>
#include <netinet/in.h>

namespace soccer {

// Pocket Soccer: tiny non-blocking UDP helper used for:
//   - lobby discovery (host broadcasts, clients listen),
//   - the join handshake (HELLO in, JOIN_OK / FULL back),
//   - match traffic (host -> clients state snapshots, clients -> host input).
//
// Host binds kHostPort (41234), clients bind kClientPort (41235) so both
// sides can broadcast/listen in the same LAN without clashing.
constexpr int kMaxPacketSize = 640;

class WifiSession {
public:
    struct Packet {
        char data[kMaxPacketSize];
        int len = 0;
        sockaddr_in from = {};
    };

    WifiSession() = default;
    ~WifiSession();

    WifiSession(const WifiSession&) = delete;
    WifiSession& operator=(const WifiSession&) = delete;

    // Binds a UDP socket on the given port and makes it non-blocking.
    bool Bind(uint16_t port);
    void Close();
    bool IsOpen() const { return fd_ >= 0; }

    bool EnableBroadcast();
    bool SendTo(const sockaddr_in& to, const void* data, int len);
    bool SendBroadcast(uint16_t destPort, const void* data, int len);
    // Non-blocking receive; returns false when nothing is pending.
    bool Receive(Packet* out);

private:
    int fd_ = -1;
};

}  // namespace soccer
