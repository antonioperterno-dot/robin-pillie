#pragma once

#include <cstdint>
#include <string>

namespace soccer {

class WifiSession {
public:
    WifiSession();
    ~WifiSession();

    bool StartHost(uint16_t port);
    bool JoinHost(const std::string& hostAddress, uint16_t port);
    void Stop();
    void Poll();
    bool IsRunning() const;
    const std::string& Status() const;

private:
    int socketFd_;
    bool running_;
    std::string status_;
};

}  // namespace soccer
