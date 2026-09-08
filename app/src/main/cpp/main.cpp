#include <android/log.h>
#include <android_native_app_glue.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include <array>
#include <cstdio>

#include "game_rules.h"
#include "wifi_session.h"

namespace {

constexpr const char* kLogTag = "PocketSoccer";

enum class LobbyState {
    kIdle,
    kHost,
    kJoin,
    kConnected,
};

struct LobbyUI {
    LobbyState state = LobbyState::kIdle;
    std::array<char, 128> line0{};
    std::array<char, 128> line1{};
    std::array<char, 128> line2{};
    std::array<char, 128> line3{};

    void Reset() {
        state = LobbyState::kIdle;
        snprintf(line0.data(), line0.size(), "POCKET SOCCER");
        snprintf(line1.data(), line1.size(), " \uC678\uACB0 \uC774\uC0B0\uC5D0\uC77C 11v11");
        snprintf(line2.data(), line2.size(), "");
        snprintf(line3.data(), line3.size(), "[H] Host  [J] Join");
    }

    void SetHost() {
        state = LobbyState::kHost;
        snprintf(line0.data(), line0.size(), "HOSTING");
        snprintf(line1.data(), line1.size(), "Share your Wi-Fi");
        snprintf(line2.data(), line2.size(), "IP address with");
        snprintf(line3.data(), line3.size(), "players on same network");
    }

    void SetJoin() {
        state = LobbyState::kJoin;
        snprintf(line0.data(), line0.size(), "JOIN");
        snprintf(line1.data(), line1.size(), "Enter host IP in");
        snprintf(line2.data(), line2.size(), "the next screen");
        snprintf(line3.data(), line3.size(), "");
    }

    void SetConnected() {
        state = LobbyState::kConnected;
        snprintf(line0.data(), line0.size(), "MATCH READY");
        snprintf(line1.data(), line1.size(), "Both phones");
        snprintf(line2.data(), line2.size(), "connected");
        snprintf(line3.data(), line3.size(), "");
    }
};

void DrawFrame(const LobbyUI& ui) {
    float r = 0.04f, g = 0.10f, b = 0.08f;
    switch (ui.state) {
        case LobbyState::kIdle:     r = 0.10f; g = 0.12f; b = 0.18f; break;
        case LobbyState::kHost:     r = 0.18f; g = 0.08f; b = 0.04f; break;
        case LobbyState::kJoin:     r = 0.12f; g = 0.04f; b = 0.18f; break;
        case LobbyState::kConnected:r = 0.04f; g = 0.10f; b = 0.08f; break;
    }
    glClearColor(r, g, b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
}

void HandleAppCommand(android_app* app, int32_t command) {
    if (command == APP_CMD_INIT_WINDOW && app->window != nullptr) {
        // frame drawn by main loop
    }
}

}  // namespace

void android_main(android_app* app) {
    app->onAppCmd = HandleAppCommand;

    soccer::WifiSession session;
    LobbyUI ui;
    ui.Reset();

    const auto assignment = soccer::AssignCharacters(2);
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "%s (%zu characters)",
                        assignment.valid ? "Rules ready" : assignment.error.c_str(),
                        assignment.characters.size());

    while (true) {
        int events = 0;
        android_poll_source* source = nullptr;
        const int timeout = app->destroyRequested ? 0 : 16;
        while (ALooper_pollOnce(timeout, nullptr, &events,
                                reinterpret_cast<void**>(&source)) >= 0) {
            if (source != nullptr) source->process(app, source);
            if (app->destroyRequested) {
                session.Stop();
                return;
            }
        }

        session.Poll();

        if (ui.state == LobbyState::kJoin && session.IsRunning()) {
            ui.SetConnected();
        }
        if (ui.state == LobbyState::kHost && session.IsRunning()) {
            ui.SetConnected();
        }

        if (!session.Status().empty() &&
            session.Status() != "Offline" &&
            session.Status() != "Wi-Fi link active") {
            if (ui.state == LobbyState::kHost) {
                snprintf(ui.line1.data(), ui.line1.size(), "%s",
                         session.Status().c_str());
            }
        }

        if (app->window != nullptr) DrawFrame(ui);
    }
}
