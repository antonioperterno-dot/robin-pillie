#include "wifi_session.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace soccer {
namespace {
constexpr const char* kHelloPacket = "POCKET_SOCCER_HELLO_V1";
constexpr const char* kHostPacket = "POCKET_SOCCER_HOST_V1";
}

WifiSession::WifiSession() : socketFd_(-1), running_(false), status_("Offline") {}
WifiSession::~WifiSession() { Stop(); }

bool WifiSession::StartHost(uint16_t port) {
    Stop();
    socketFd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (socketFd_ < 0) {
        status_ = "Could not create Wi-Fi socket";
        return false;
    }

    int reuse = 1;
    setsockopt(socketFd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);
    if (bind(socketFd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        Stop();
        status_ = "Could not open host port";
        return false;
    }
    fcntl(socketFd_, F_SETFL, O_NONBLOCK);
    running_ = true;
    status_ = "Host ready - share your Wi-Fi address";
    return true;
}

bool WifiSession::JoinHost(const std::string& hostAddress, uint16_t port) {
    Stop();
    socketFd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (socketFd_ < 0) {
        status_ = "Could not create Wi-Fi socket";
        return false;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, hostAddress.c_str(), &address.sin_addr) != 1) {
        Stop();
        status_ = "Enter a valid host address";
        return false;
    }
    sendto(socketFd_, kHelloPacket, sizeof(kHelloPacket), 0,
           reinterpret_cast<sockaddr*>(&address), sizeof(address));
    fcntl(socketFd_, F_SETFL, O_NONBLOCK);
    running_ = true;
    status_ = "Joining host...";
    return true;
}

void WifiSession::Stop() {
    if (socketFd_ >= 0) close(socketFd_);
    socketFd_ = -1;
    running_ = false;
    status_ = "Offline";
}

void WifiSession::Poll() {
    if (!running_) return;
    char packet[128]{};
    sockaddr_in sender{};
    socklen_t senderLength = sizeof(sender);
    const auto bytes = recvfrom(socketFd_, packet, sizeof(packet) - 1, 0,
                                reinterpret_cast<sockaddr*>(&sender), &senderLength);
    if (bytes <= 0) return;

    if (std::string(packet, static_cast<size_t>(bytes)) == kHelloPacket) {
        status_ = "Wi-Fi link active";
        sendto(socketFd_, kHostPacket, sizeof(kHostPacket), 0,
               reinterpret_cast<sockaddr*>(&sender), senderLength);
    }
}

bool WifiSession::IsRunning() const { return running_; }
const std::string& WifiSession::Status() const { return status_; }

}  // namespace soccer
