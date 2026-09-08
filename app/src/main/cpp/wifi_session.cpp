#include "wifi_session.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace soccer {

WifiSession::~WifiSession() { Close(); }

bool WifiSession::Bind(uint16_t port) {
    Close();
    fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) return false;

    int reuse = 1;
    setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);
    if (bind(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        Close();
        return false;
    }
    fcntl(fd_, F_SETFL, O_NONBLOCK);
    return true;
}

void WifiSession::Close() {
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
}

bool WifiSession::EnableBroadcast() {
    if (fd_ < 0) return false;
    int on = 1;
    return setsockopt(fd_, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on)) == 0;
}

bool WifiSession::SendTo(const sockaddr_in& to, const void* data, int len) {
    if (fd_ < 0 || data == nullptr || len <= 0) return false;
    const ssize_t sent = sendto(fd_, data, static_cast<size_t>(len), 0,
                                reinterpret_cast<const sockaddr*>(&to), sizeof(to));
    return sent == static_cast<ssize_t>(len);
}

bool WifiSession::SendBroadcast(uint16_t destPort, const void* data, int len) {
    sockaddr_in to = {};
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    to.sin_port = htons(destPort);
    return SendTo(to, data, len);
}

bool WifiSession::Receive(Packet* out) {
    if (fd_ < 0 || out == nullptr) return false;
    socklen_t fromLen = sizeof(out->from);
    const ssize_t n = recvfrom(fd_, out->data, kMaxPacketSize - 1, 0,
                               reinterpret_cast<sockaddr*>(&out->from), &fromLen);
    if (n <= 0) return false;
    out->len = static_cast<int>(n);
    out->data[n] = '\0';
    return true;
}

}  // namespace soccer
