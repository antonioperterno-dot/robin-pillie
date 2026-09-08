// Pocket Soccer: native C++ Android entry point.
//
// One phone hosts a Wi-Fi UDP lobby (11v11, 2-22 phones); friends join from
// the same lobby. The host runs the match simulation (game_world) while
// clients render the host's state snapshots. Touch UI: lobby buttons,
// left-side virtual joystick, right-side KICK button.
#include <android/input.h>
#include <android/log.h>
#include <android_native_app_glue.h>

#include <arpa/inet.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "game_rules.h"
#include "game_world.h"
#include "renderer.h"
#include "wifi_session.h"

namespace soccer {
namespace {

constexpr const char* kLogTag = "PocketSoccer";
constexpr uint16_t kHostPort = 41234;
constexpr uint16_t kClientPort = 41235;
constexpr int kHdrLen = 9;

// 9-byte packet ids (no trailing NUL on the wire).
const char kPktLobby[] = "PS_LOBBY1";  // host -> broadcast: phoneCount, joined
const char kPktHello[] = "PS_HELLO1";  // client -> host: wants to join
const char kPktJoinOk[] = "PS_JOINOK";  // host -> client: count, phoneId
const char kPktFull[] = "PS_FULLV1";   // host -> client: match is full
const char kPktState[] = "PS_STATE1";  // host -> clients: match snapshot
const char kPktInput[] = "PS_INPUT1";  // client -> host: phoneId, dx, dy, kick

enum class Screen : uint8_t { kLobby, kMatch };
enum class LobbyMode : uint8_t {
    kIdle,
    kHosting,
    kJoining,
    kJoined,
    kFull,
    kLost,
};

struct Button {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;  // pixel coords, x/y = top-left corner
    bool Hit(float px, float py) const {
        return px >= x && px <= x + w && py >= y && py <= y + h;
    }
};

struct ClientSlot {
    sockaddr_in addr = {};
    bool used = false;
};

struct App {
    android_app* android = nullptr;
    Renderer renderer;
    WifiSession net;
    MatchSim sim;
    MatchState snapshot;  // client: latest host state snapshot
    bool hasSnapshot = false;

    PhoneInput inputs[kMaxPhones];
    ClientSlot clients[kMaxPhones];

    Screen screen = Screen::kLobby;
    LobbyMode lobby = LobbyMode::kIdle;
    bool isHost = false;
    bool matchStarted = false;

    int phoneCount = 2;  // host: target phones in the match
    int joined = 0;      // host: remote phones that joined

    // Host timing.
    float beaconTimer = 0.0f;
    float stateTimer = 0.0f;
    float simAccum = 0.0f;

    // Client state.
    sockaddr_in hostAddr = {};
    char hostIp[32] = "?";
    int hostCount = 0;
    bool hasHost = false;
    float helloTimer = 0.0f;
    float inputTimer = 0.0f;
    float sinceState = 0.0f;
    int myPhoneId = -1;
    int myChars[kTeamSize] = {0};
    int myCharCount = 0;

    // Touch controls.
    float joyOriginX = 0.0f;
    float joyOriginY = 0.0f;
    float joyDx = 0.0f;
    float joyDy = 0.0f;
    bool kickHeld = false;
    int32_t joyPointer = -1;
    int32_t kickPointer = -1;

    // Button layout, recomputed every frame during Draw.
    Button btnHost, btnJoin, btnMinus, btnPlus, btnStart, btnBack;
    Button btnJoinGo, btnKick, btnEnd, btnRematch;

    float uiTime = 0.0f;
    std::chrono::steady_clock::time_point lastFrame;
};

}  // namespace

// --- packet encode/decode helpers ---------------------------------------
void PutU8(char* b, int& o, uint8_t v) { b[o++] = static_cast<char>(v); }
void PutF32(char* b, int& o, float v) {
    std::memcpy(b + o, &v, sizeof(float));
    o += static_cast<int>(sizeof(float));
}
uint8_t GetU8(const char* b, int& o) { return static_cast<uint8_t>(b[o++]); }
float GetF32(const char* b, int& o) {
    float v = 0.0f;
    std::memcpy(&v, b + o, sizeof(float));
    o += static_cast<int>(sizeof(float));
    return v;
}

int BuildLobby(char* buf, int count, int joined) {
    int o = kHdrLen;
    std::memcpy(buf, kPktLobby, kHdrLen);
    PutU8(buf, o, static_cast<uint8_t>(count));
    PutU8(buf, o, static_cast<uint8_t>(joined));
    return o;
}

int BuildJoinOk(char* buf, int count, int phoneId) {
    int o = kHdrLen;
    std::memcpy(buf, kPktJoinOk, kHdrLen);
    PutU8(buf, o, static_cast<uint8_t>(count));
    PutU8(buf, o, static_cast<uint8_t>(phoneId));
    return o;
}

int BuildState(char* buf, const MatchState& s) {
    int o = kHdrLen;
    std::memcpy(buf, kPktState, kHdrLen);
    PutF32(buf, o, s.ballX);
    PutF32(buf, o, s.ballY);
    PutF32(buf, o, s.timeLeft);
    PutU8(buf, o, static_cast<uint8_t>(s.phase));
    PutU8(buf, o, s.score[0]);
    PutU8(buf, o, s.score[1]);
    PutU8(buf, o, static_cast<uint8_t>(s.phoneCount));
    for (int p = 0; p < s.phoneCount; ++p) {
        const int id = s.activeChar[p];
        PutU8(buf, o, static_cast<uint8_t>(id < 0 ? 255 : id));
    }
    for (int i = 0; i < kTeamSize * 2; ++i) {
        PutF32(buf, o, s.chars[i].x);
        PutF32(buf, o, s.chars[i].y);
    }
    return o;
}

int BuildInput(char* buf, int phoneId, const PhoneInput& in) {
    int o = kHdrLen;
    std::memcpy(buf, kPktInput, kHdrLen);
    PutU8(buf, o, static_cast<uint8_t>(phoneId));
    PutF32(buf, o, in.dx);
    PutF32(buf, o, in.dy);
    PutU8(buf, o, in.kick ? 1 : 0);
    return o;
}

bool DecodeState(const WifiSession::Packet& pkt, MatchState* out) {
    // ballX/Y + timeLeft (12) + phase (1) + scores (2) + phoneCount (1).
    const int minLen = kHdrLen + 16;
    if (out == nullptr || pkt.len < minLen) return false;
    int o = kHdrLen;
    out->ballX = GetF32(pkt.data, o);
    out->ballY = GetF32(pkt.data, o);
    out->timeLeft = GetF32(pkt.data, o);
    out->phase = static_cast<MatchPhase>(GetU8(pkt.data, o));
    out->score[0] = GetU8(pkt.data, o);
    out->score[1] = GetU8(pkt.data, o);
    out->phoneCount = static_cast<int>(GetU8(pkt.data, o));
    if (out->phoneCount < kMinPhones || out->phoneCount > kMaxPhones) return false;
    const int need =
        kHdrLen + 16 + out->phoneCount + (kTeamSize * 2) * 8;
    if (pkt.len < need) return false;
    for (int p = 0; p < kMaxPhones; ++p) {
        if (p < out->phoneCount) {
            // 255 is the "none assigned" sentinel emitted by the host.
            const int raw = static_cast<int>(GetU8(pkt.data, o));
            out->activeChar[p] = raw == 255 ? -1 : raw;
        } else {
            out->activeChar[p] = -1;
        }
    }
    for (int i = 0; i < kTeamSize * 2; ++i) {
        out->chars[i].x = GetF32(pkt.data, o);
        out->chars[i].y = GetF32(pkt.data, o);
        out->chars[i].vx = 0.0f;
        out->chars[i].vy = 0.0f;
        out->chars[i].team = i < kTeamSize ? 0 : 1;
    }
    out->ballVx = 0.0f;
    out->ballVy = 0.0f;
    return true;
}
// --- host flow ------------------------------------------------------------
void Teardown(App& app) {
    app.net.Close();
    app.isHost = false;
    app.matchStarted = false;
    app.hasSnapshot = false;
    app.hasHost = false;
    app.myPhoneId = -1;
    app.myCharCount = 0;
    app.joined = 0;
    app.phoneCount = 2;
    app.joyDx = 0.0f;
    app.joyDy = 0.0f;
    app.kickHeld = false;
    app.joyPointer = -1;
    app.kickPointer = -1;
    for (auto& slot : app.clients) slot.used = false;
    for (auto& in : app.inputs) in = PhoneInput{};
    app.screen = Screen::kLobby;
    app.lobby = LobbyMode::kIdle;
}

void StartHosting(App& app) {
    Teardown(app);
    if (!app.net.Bind(kHostPort)) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "host bind failed");
        return;
    }
    app.net.EnableBroadcast();
    app.isHost = true;
    app.beaconTimer = 0.0f;
    app.lobby = LobbyMode::kHosting;
}

void StartMatch(App& app) {
    app.sim.Start(app.phoneCount);
    app.matchStarted = true;
    app.simAccum = 0.0f;
    app.stateTimer = 0.0f;
    for (auto& in : app.inputs) in = PhoneInput{};
    app.screen = Screen::kMatch;
}

void HostTick(App& app, float dt) {
    // Advertise the lobby while waiting for everyone to join.
    if (!app.matchStarted) {
        app.beaconTimer -= dt;
        if (app.beaconTimer <= 0.0f) {
            char buf[16];
            const int n = BuildLobby(buf, app.phoneCount, app.joined);
            app.net.SendBroadcast(kClientPort, buf, n);
            app.beaconTimer = 1.0f;
        }
    }

    WifiSession::Packet pkt;
    while (app.net.Receive(&pkt)) {
        if (pkt.len < kHdrLen) continue;
        if (std::memcmp(pkt.data, kPktHello, kHdrLen) == 0) {
            char buf[16];
            if (app.joined < app.phoneCount - 1) {
                const int phoneId = app.joined + 1;
                app.clients[phoneId].addr = pkt.from;
                app.clients[phoneId].used = true;
                app.joined++;
                const int n = BuildJoinOk(buf, app.phoneCount, phoneId);
                app.net.SendTo(pkt.from, buf, n);
                __android_log_print(ANDROID_LOG_INFO, kLogTag,
                                    "phone %d joined (%s)", phoneId,
                                    inet_ntoa(pkt.from.sin_addr));
            } else {
                std::memcpy(buf, kPktFull, kHdrLen);
                app.net.SendTo(pkt.from, buf, kHdrLen);
            }
        } else if (std::memcmp(pkt.data, kPktInput, kHdrLen) == 0 &&
                   pkt.len >= kHdrLen + 10) {
            int o = kHdrLen;
            const int phoneId = static_cast<int>(GetU8(pkt.data, o));
            if (phoneId > 0 && phoneId < kMaxPhones) {
                PhoneInput in;
                in.dx = GetF32(pkt.data, o);
                in.dy = GetF32(pkt.data, o);
                in.kick = GetU8(pkt.data, o) != 0;
                app.inputs[phoneId] = in;
            }
        }
    }

    if (!app.matchStarted) return;

    // Local touch feeds phone 0.
    app.inputs[0].dx = app.joyDx;
    app.inputs[0].dy = app.joyDy;
    app.inputs[0].kick = app.kickHeld;

    app.simAccum += dt;
    const float stepDt = 1.0f / 60.0f;
    int steps = 0;
    while (app.simAccum >= stepDt && steps < 4) {
        app.sim.Step(stepDt, app.inputs);
        app.simAccum -= stepDt;
        ++steps;
    }

    app.stateTimer -= dt;
    if (app.stateTimer <= 0.0f) {
        char buf[kMaxPacketSize];
        const int n = BuildState(buf, app.sim.state());
        for (int p = 1; p < kMaxPhones; ++p) {
            if (app.clients[p].used) app.net.SendTo(app.clients[p].addr, buf, n);
        }
        app.stateTimer = 0.05f;
    }
}
// --- client flow ----------------------------------------------------------
void StartJoining(App& app) {
    Teardown(app);
    if (!app.net.Bind(kClientPort)) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "client bind failed");
        return;
    }
    app.lobby = LobbyMode::kJoining;
    app.helloTimer = 0.0f;
}

void SendHello(App& app) {
    char buf[16];
    std::memcpy(buf, kPktHello, kHdrLen);
    app.net.SendTo(app.hostAddr, buf, kHdrLen);
}

void ClientTick(App& app, float dt) {
    WifiSession::Packet pkt;
    while (app.net.Receive(&pkt)) {
        if (pkt.len < kHdrLen) continue;
        if (std::memcmp(pkt.data, kPktLobby, kHdrLen) == 0 &&
            pkt.len >= kHdrLen + 2 && app.myPhoneId < 0) {
            int o = kHdrLen;
            app.hostCount = static_cast<int>(GetU8(pkt.data, o));
            app.hostAddr = pkt.from;
            std::snprintf(app.hostIp, sizeof(app.hostIp), "%s",
                          inet_ntoa(pkt.from.sin_addr));
            app.hasHost = true;
        } else if (std::memcmp(pkt.data, kPktJoinOk, kHdrLen) == 0 &&
                   pkt.len >= kHdrLen + 2) {
            int o = kHdrLen;
            const int count = static_cast<int>(GetU8(pkt.data, o));
            const int pid = static_cast<int>(GetU8(pkt.data, o));
            if (!IsSupportedPhoneCount(count) || pid < 0 || pid >= count) {
                continue;  // malformed: keep listening for a good one
            }
            app.myPhoneId = pid;
            app.phoneCount = count;
            app.myCharCount = 0;
            for (const auto& ch : assignment.characters) {
                if (ch.phoneId == app.myPhoneId && app.myCharCount < kTeamSize) {
                    app.myChars[app.myCharCount++] = ch.characterId;
                }
            }
            app.lobby = LobbyMode::kJoined;
        } else if (std::memcmp(pkt.data, kPktFull, kHdrLen) == 0) {
            app.lobby = LobbyMode::kFull;
        } else if (std::memcmp(pkt.data, kPktState, kHdrLen) == 0) {
            MatchState s;
            if (DecodeState(pkt, &s)) {
                app.snapshot = s;
                app.hasSnapshot = true;
                app.sinceState = 0.0f;
                if (app.screen != Screen::kMatch) {
                    app.screen = Screen::kMatch;
                    app.joyDx = 0.0f;
                    app.joyDy = 0.0f;
                    app.kickHeld = false;
                }
            }
        }
    }

    // (Re)send HELLO until the host answers with JOIN_OK.
    if (app.lobby == LobbyMode::kJoining && app.hasHost) {
        app.helloTimer -= dt;
        if (app.helloTimer <= 0.0f) {
            SendHello(app);
            app.helloTimer = 0.4f;
        }
    }

    if (app.screen == Screen::kMatch && app.myPhoneId >= 0) {
        app.sinceState += dt;
        if (app.sinceState > 3.0f) {
            // Host went quiet: back to the lobby with a "lost" notice.
            Teardown(app);
            app.lobby = LobbyMode::kLost;
            return;
        }
        app.inputTimer -= dt;
        if (app.inputTimer <= 0.0f) {
            char buf[32];
            PhoneInput in;
            in.dx = app.joyDx;
            in.dy = app.joyDy;
            in.kick = app.kickHeld;
            const int n = BuildInput(buf, app.myPhoneId, in);
            app.net.SendTo(app.hostAddr, buf, n);
            app.inputTimer = 0.033f;
        }
    }
}
// --- touch input ----------------------------------------------------------
void PressLobby(App& app, float x, float y) {
    switch (app.lobby) {
        case LobbyMode::kIdle:
            if (app.btnHost.Hit(x, y)) {
                StartHosting(app);
            } else if (app.btnJoin.Hit(x, y)) {
                StartJoining(app);
            }
            break;
        case LobbyMode::kHosting:
            if (app.btnMinus.Hit(x, y)) {
                if (app.joined == 0 && app.phoneCount > kMinPhones) {
                    app.phoneCount -= 2;
                }
            } else if (app.btnPlus.Hit(x, y)) {
                if (app.joined == 0 && app.phoneCount < kMaxPhones) {
                    app.phoneCount += 2;
                }
            } else if (app.btnStart.Hit(x, y)) {
                if (!app.matchStarted && app.joined >= app.phoneCount - 1) {
                    StartMatch(app);
                }
            } else if (app.btnBack.Hit(x, y)) {
                Teardown(app);
            }
            break;
        case LobbyMode::kJoining:
            if (app.btnJoinGo.Hit(x, y)) {
                if (app.hasHost) app.helloTimer = 0.0f;  // ping now
            } else if (app.btnBack.Hit(x, y)) {
                Teardown(app);
            }
            break;
        case LobbyMode::kJoined:
        case LobbyMode::kFull:
        case LobbyMode::kLost:
            if (app.btnBack.Hit(x, y)) Teardown(app);
            break;
    }
}

int32_t HandleInput(android_app* androidApp, AInputEvent* event) {
    App& app = *static_cast<App*>(androidApp->userData);
    if (AInputEvent_getType(event) != AINPUT_EVENT_TYPE_MOTION) return 0;
    const float w = static_cast<float>(app.renderer.Width());
    const float h = static_cast<float>(app.renderer.Height());
    if (w <= 0.0f || h <= 0.0f) return 0;

    const int32_t action = AMotionEvent_getAction(event);
    const int32_t masked = action & AMOTION_EVENT_ACTION_MASK;
    const int32_t index =
        (action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >>
        AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;

    if (masked == AMOTION_EVENT_ACTION_DOWN ||
        masked == AMOTION_EVENT_ACTION_POINTER_DOWN) {
        const int32_t id =
            AMotionEvent_getPointerId(event, static_cast<size_t>(index));
        const float x = AMotionEvent_getX(event, index);
        const float y = AMotionEvent_getY(event, index);
        if (app.screen == Screen::kLobby) {
            PressLobby(app, x, y);
            return 1;
        }
        // Match screen: host END / REMATCH, then KICK, then left stick.
        if (app.isHost && app.btnEnd.Hit(x, y)) {
            Teardown(app);
            return 1;
        }
        if (app.isHost && !app.matchStarted) return 1;
        if (app.isHost && app.sim.state().phase == MatchPhase::kFullTime &&
            app.btnRematch.Hit(x, y)) {
            app.sim.Start(app.phoneCount);
            return 1;
        }
        if (app.btnKick.Hit(x, y)) {
            app.kickPointer = id;
            app.kickHeld = true;
        } else if (x < w * 0.45f) {
            app.joyPointer = id;
            app.joyOriginX = x;
            app.joyOriginY = y;
            app.joyDx = 0.0f;
            app.joyDy = 0.0f;
        }
        return 1;
    }

    if (masked == AMOTION_EVENT_ACTION_MOVE) {
        constexpr float kMaxR = 70.0f;
        const size_t n = AMotionEvent_getPointerCount(event);
        for (size_t i = 0; i < n; ++i) {
            if (AMotionEvent_getPointerId(event, i) != app.joyPointer) continue;
            float dx = AMotionEvent_getX(event, i) - app.joyOriginX;
            float dy = AMotionEvent_getY(event, i) - app.joyOriginY;
            const float len = std::sqrt(dx * dx + dy * dy);
            if (len > kMaxR) {
                dx *= kMaxR / len;
                dy *= kMaxR / len;
            }
            app.joyDx = dx / kMaxR;
            app.joyDy = -dy / kMaxR;  // screen y is down, field y is up
            if (std::fabs(app.joyDx) < 0.12f && std::fabs(app.joyDy) < 0.12f) {
                app.joyDx = 0.0f;
                app.joyDy = 0.0f;
            }
        }
        return 1;
    }

    if (masked == AMOTION_EVENT_ACTION_UP ||
        masked == AMOTION_EVENT_ACTION_POINTER_UP) {
        const size_t upIndex =
            masked == AMOTION_EVENT_ACTION_UP
                ? 0
                : static_cast<size_t>(index < 0 ? 0 : index);
        const int32_t id = AMotionEvent_getPointerId(event, upIndex);
        if (id == app.joyPointer) {
            app.joyPointer = -1;
            app.joyDx = 0.0f;
            app.joyDy = 0.0f;
        }
        if (id == app.kickPointer) {
            app.kickPointer = -1;
            app.kickHeld = false;
        }
        return 1;
    }
    return 0;
}
// --- lobby UI -------------------------------------------------------------
Button MakeButton(App& app, float cx, float cy, float bw, float bh,
                  const char* label, float ps = 4.0f) {
    Renderer& r = app.renderer;
    r.Rect(cx, cy, bw, bh, 0.16f, 0.38f, 0.24f, 0.92f);
    r.Text(cx - r.TextWidth(label, ps) * 0.5f, cy - 2.5f * ps, label, ps,
           0.96f, 0.98f, 0.90f);
    Button b;
    b.x = cx - bw * 0.5f;
    b.y = cy - bh * 0.5f;
    b.w = bw;
    b.h = bh;
    return b;
}

void Centered(App& app, const char* text, float ps, float y,
              float r, float g, float b, float a = 1.0f) {
    Renderer& renderer = app.renderer;
    const float w = static_cast<float>(renderer.Width());
    renderer.Text(w * 0.5f - renderer.TextWidth(text, ps) * 0.5f, y, text, ps,
                  r, g, b, a);
}

void DrawLobby(App& app) {
    Renderer& r = app.renderer;
    r.SetPixelView();
    const float w = static_cast<float>(r.Width());
    const float h = static_cast<float>(r.Height());
    char buf[64];

    if (app.lobby == LobbyMode::kIdle) {
        Centered(app, "POCKET SOCCER", 7.0f, h * 0.12f, 0.93f, 0.96f, 0.35f);
        Centered(app, "11 V 11 OVER LOCAL WI-FI", 3.0f, h * 0.12f + 46.0f,
                 0.70f, 0.78f, 0.62f);
        Centered(app, "ONE PHONE PER PLAYER, 2-22 PHONES", 3.0f,
                 h * 0.12f + 64.0f, 0.55f, 0.62f, 0.52f);
        app.btnHost = MakeButton(app, w * 0.28f, h * 0.55f, w * 0.34f,
                                 h * 0.16f, "HOST");
        app.btnJoin = MakeButton(app, w * 0.72f, h * 0.55f, w * 0.34f,
                                 h * 0.16f, "JOIN");
        Centered(app, "HOST: START A MATCH  /  JOIN: FIND THE HOST", 3.0f,
                 h * 0.80f, 0.60f, 0.66f, 0.55f);
        return;
    }

    if (app.lobby == LobbyMode::kHosting) {
        Centered(app, "HOSTING - SAME WI-FI", 5.0f, h * 0.08f, 1.0f, 1.0f,
                 0.5f);
        std::snprintf(buf, sizeof(buf), "PHONES: %d", app.phoneCount);
        Centered(app, buf, 6.0f, h * 0.26f, 1.0f, 1.0f, 1.0f);
        app.btnMinus = MakeButton(app, w * 0.26f, h * 0.30f, w * 0.14f,
                                  h * 0.11f, "-", 6.0f);
        app.btnPlus = MakeButton(app, w * 0.74f, h * 0.30f, w * 0.14f,
                                 h * 0.11f, "+", 6.0f);
        std::snprintf(buf, sizeof(buf), "JOINED: %d / %d", app.joined,
                      app.phoneCount - 1);
        Centered(app, buf, 4.0f, h * 0.46f, 0.85f, 0.95f, 0.85f);
        if (app.joined == 0) {
            Centered(app, "TAP PLUS/MINUS TO SET PHONE COUNT", 3.0f,
                     h * 0.55f, 0.60f, 0.66f, 0.55f);
        } else {
            Centered(app, "PHONE COUNT LOCKED - PLAYERS JOINED", 3.0f,
                     h * 0.55f, 0.60f, 0.66f, 0.55f);
        }
        app.btnStart = MakeButton(app, w * 0.5f, h * 0.68f, w * 0.52f,
                                  h * 0.13f, "START MATCH");
        app.btnBack = MakeButton(app, w * 0.5f, h * 0.86f, w * 0.30f,
                                 h * 0.10f, "BACK");
        return;
    }

    if (app.lobby == LobbyMode::kJoining) {
        Centered(app, "JOIN MATCH", 6.0f, h * 0.10f, 1.0f, 1.0f, 0.5f);
        if (!app.hasHost) {
            const bool blink = static_cast<int>(app.uiTime * 2.0f) % 2 == 0;
            Centered(app, "SCANNING FOR HOST...", 4.0f, h * 0.40f, 1.0f, 1.0f,
                     1.0f, blink ? 1.0f : 0.25f);
            Centered(app, "JOIN THE HOST'S WI-FI NETWORK", 3.0f, h * 0.50f,
                     0.60f, 0.66f, 0.55f);
            app.btnJoinGo = Button{};  // inert until a beacon arrives
        } else {
            std::snprintf(buf, sizeof(buf), "HOST FOUND: %s", app.hostIp);
            Centered(app, buf, 4.0f, h * 0.32f, 0.85f, 0.95f, 0.85f);
            std::snprintf(buf, sizeof(buf), "MATCH SIZE: %d PHONES",
                          app.hostCount);
            Centered(app, buf, 4.0f, h * 0.42f, 0.85f, 0.95f, 0.85f);
            app.btnJoinGo = MakeButton(app, w * 0.5f, h * 0.62f, w * 0.40f,
                                       h * 0.13f, "JOIN");
        }
        app.btnBack = MakeButton(app, w * 0.5f, h * 0.86f, w * 0.30f,
                                 h * 0.10f, "BACK");
        return;
    }

    if (app.lobby == LobbyMode::kJoined) {
        Centered(app, "CONNECTED", 6.0f, h * 0.12f, 0.60f, 1.0f, 0.60f);
        std::snprintf(buf, sizeof(buf), "YOU ARE PHONE %d OF %d",
                      app.myPhoneId, app.phoneCount);
        Centered(app, buf, 4.0f, h * 0.32f, 1.0f, 1.0f, 1.0f);
        std::snprintf(buf, sizeof(buf), "YOUR CHARACTERS: %d", app.myCharCount);
        Centered(app, buf, 4.0f, h * 0.42f, 1.0f, 1.0f, 1.0f);
        Centered(app, "WAITING FOR THE HOST TO START...", 3.0f, h * 0.54f,
                 0.70f, 0.78f, 0.62f);
        app.btnBack = MakeButton(app, w * 0.5f, h * 0.86f, w * 0.30f,
                                 h * 0.10f, "BACK");
        return;
    }

    if (app.lobby == LobbyMode::kFull) {
        Centered(app, "MATCH IS FULL", 5.0f, h * 0.30f, 1.0f, 0.55f, 0.45f);
        Centered(app, "ASK THE HOST FOR MORE PHONES", 3.0f, h * 0.42f,
                 0.70f, 0.78f, 0.62f);
        app.btnBack = MakeButton(app, w * 0.5f, h * 0.70f, w * 0.30f,
                                 h * 0.10f, "BACK");
        return;
    }

    // kLost
    Centered(app, "CONNECTION LOST", 5.0f, h * 0.30f, 1.0f, 0.55f, 0.45f);
    Centered(app, "THE HOST STOPPED SENDING MATCH DATA", 3.0f, h * 0.42f,
             0.70f, 0.78f, 0.62f);
    app.btnBack = MakeButton(app, w * 0.5f, h * 0.70f, w * 0.30f, h * 0.10f,
                             "BACK");
}
// --- match view -----------------------------------------------------------
void DrawPitch(App& app) {
    Renderer& r = app.renderer;
    r.SetFieldView();

    // Grass with alternating mow stripes.
    r.Rect(0.0f, 0.0f, 2.0f * kFieldHalfW + 6.0f, 2.0f * kFieldHalfH + 6.0f,
           0.07f, 0.32f, 0.14f);
    const float stripeW = 2.0f * kFieldHalfW / 8.0f;
    for (int i = 0; i < 8; i += 2) {
        r.Rect(-kFieldHalfW + (i + 0.5f) * stripeW, 0.0f, stripeW,
               2.0f * kFieldHalfH + 6.0f, 0.085f, 0.36f, 0.16f);
    }

    constexpr float kLine = 0.35f;
    constexpr float kLw = 0.92f;
    // Touch lines.
    r.Rect(0.0f, -kFieldHalfH, 2.0f * kFieldHalfW + kLine, kLine, kLw, kLw,
           kLw);
    r.Rect(0.0f, kFieldHalfH, 2.0f * kFieldHalfW + kLine, kLine, kLw, kLw,
           kLw);
    r.Rect(-kFieldHalfW, 0.0f, kLine, 2.0f * kFieldHalfH, kLw, kLw, kLw);
    r.Rect(kFieldHalfW, 0.0f, kLine, 2.0f * kFieldHalfH, kLw, kLw, kLw);
    // Halfway + center.
    r.Rect(0.0f, 0.0f, kLine, 2.0f * kFieldHalfH, kLw, kLw, kLw);
    r.Ring(0.0f, 0.0f, 9.15f, kLine, kLw, kLw, kLw);
    r.Circle(0.0f, 0.0f, 0.5f, kLw, kLw, kLw);
    // Penalty boxes (16.5 m deep, 40.3 m wide).
    for (int side = 0; side < 2; ++side) {
        const float s = side == 0 ? -1.0f : 1.0f;
        const float frontX = s * (kFieldHalfW - 16.5f);
        const float midX = s * (kFieldHalfW - 16.5f / 2.0f);
        r.Rect(frontX, 0.0f, kLine, 40.3f, kLw, kLw, kLw);
        r.Rect(midX, -20.15f, 16.5f, kLine, kLw, kLw, kLw);
        r.Rect(midX, 20.15f, 16.5f, kLine, kLw, kLw, kLw);
        // Goals.
        r.Rect(s * (kFieldHalfW + 1.2f), 0.0f, 2.4f, 2.0f * kGoalHalfW, 0.95f,
               0.95f, 0.95f);
    }
}
void DrawMatch(App& app) {
    Renderer& r = app.renderer;
    const MatchState* s =
        app.isHost ? &app.sim.state() : (app.hasSnapshot ? &app.snapshot
                                                         : nullptr);
    if (s == nullptr) {  // client before the first snapshot arrives
        r.SetPixelView();
        Centered(app, "WAITING FOR MATCH DATA...", 4.0f,
                 static_cast<float>(r.Height()) * 0.45f, 1.0f, 1.0f, 1.0f);
        return;
    }

    DrawPitch(app);
    r.SetFieldView();

    // Characters: red attacks +x, blue attacks -x.
    for (int i = 0; i < kTeamSize * 2; ++i) {
        const MatchChar& c = s->chars[i];
        if (c.team == 0) {
            r.Circle(c.x, c.y, kCharRadius, 0.92f, 0.28f, 0.22f);
        } else {
            r.Circle(c.x, c.y, kCharRadius, 0.25f, 0.45f, 0.95f);
        }
        r.Ring(c.x, c.y, kCharRadius, 0.22f, 1.0f, 1.0f, 1.0f, 0.25f);
    }

    // Highlight the character this phone currently controls.
    int highlight = -1;
    if (app.isHost && s->phoneCount > 0) {
        highlight = s->activeChar[0];
    } else if (!app.isHost && app.myPhoneId >= 0 &&
               app.myPhoneId < s->phoneCount) {
        highlight = s->activeChar[app.myPhoneId];
    }
    if (highlight >= 0 && highlight < kTeamSize * 2) {
        const MatchChar& c = s->chars[highlight];
        r.Ring(c.x, c.y, kCharRadius + 0.9f, 0.5f, 1.0f, 1.0f, 0.5f, 0.95f);
    }

    r.Circle(s->ballX, s->ballY, kBallRadius, 0.98f, 0.98f, 0.95f);

    // --- HUD overlay ------------------------------------------------------
    r.SetPixelView();
    const float w = static_cast<float>(r.Width());
    const float h = static_cast<float>(r.Height());
    char buf[64];

    std::snprintf(buf, sizeof(buf), "%d - %d", s->score[0], s->score[1]);
    Centered(app, buf, 8.0f, h * 0.02f, 1.0f, 1.0f, 1.0f);

    const int secs =
        static_cast<int>(s->timeLeft < 0.0f ? 0.0f : s->timeLeft);
    std::snprintf(buf, sizeof(buf), "%d:%02d", secs / 60, secs % 60);
    r.Text(w - r.TextWidth(buf, 5.0f) - 14.0f, h * 0.03f, buf, 5.0f, 1.0f,
           1.0f, 1.0f, 0.9f);

    if (s->phase == MatchPhase::kKickoff) {
        std::snprintf(buf, sizeof(buf), "KICKOFF %d",
                      static_cast<int>(std::ceil(s->phaseTimer)));
        Centered(app, buf, 6.0f, h * 0.42f, 1.0f, 1.0f, 0.4f);
    } else if (s->phase == MatchPhase::kGoal) {
        Centered(app, "GOAL!", 9.0f, h * 0.42f, 1.0f, 1.0f, 0.3f);
    } else if (s->phase == MatchPhase::kFullTime) {
        Centered(app, "FULL TIME", 8.0f, h * 0.36f, 1.0f, 1.0f, 0.4f);
    }

    // Left virtual stick.
    const float jx = app.joyPointer >= 0 ? app.joyOriginX : w * 0.16f;
    const float jy = app.joyPointer >= 0 ? app.joyOriginY : h * 0.76f;
    r.Ring(jx, jy, 64.0f, 4.0f, 1.0f, 1.0f, 1.0f, 0.30f);
    r.Circle(jx + app.joyDx * 46.0f, jy - app.joyDy * 46.0f, 20.0f, 1.0f,
             1.0f, 1.0f, 0.55f);

    // Right KICK button.
    app.btnKick.x = w - 150.0f;
    app.btnKick.y = h - 150.0f;
    app.btnKick.w = 120.0f;
    app.btnKick.h = 120.0f;
    r.Circle(w - 90.0f, h - 90.0f, 56.0f, 0.85f, 0.25f, 0.20f,
             app.kickHeld ? 0.95f : 0.60f);
    r.Text(w - 90.0f - r.TextWidth("KICK", 5.0f) * 0.5f, h - 90.0f - 12.5f,
           "KICK", 5.0f, 1.0f, 1.0f, 1.0f);

    if (app.isHost) {
        // Small END button (top-left) aborts the match for everyone.
        const char* endLabel = "END";
        r.Rect(48.0f, 32.0f, 76.0f, 44.0f, 0.20f, 0.20f, 0.25f, 0.80f);
        r.Text(48.0f - r.TextWidth(endLabel, 4.0f) * 0.5f, 32.0f - 10.0f,
               endLabel, 4.0f, 1.0f, 1.0f, 1.0f);
        app.btnEnd.x = 10.0f;
        app.btnEnd.y = 10.0f;
        app.btnEnd.w = 76.0f;
        app.btnEnd.h = 44.0f;
        if (s->phase == MatchPhase::kFullTime) {
            app.btnRematch = MakeButton(app, w * 0.5f, h * 0.58f, w * 0.50f,
                                        h * 0.13f, "REMATCH");
        }
    }
}
// --- glue -----------------------------------------------------------------
void HandleAppCmd(android_app* androidApp, int32_t cmd) {
    App& app = *static_cast<App*>(androidApp->userData);
    switch (cmd) {
        case APP_CMD_INIT_WINDOW:
            if (!app.renderer.Prepare(androidApp)) {
                __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                                    "renderer prepare failed");
            }
            break;
        case APP_CMD_TERM_WINDOW:
            app.renderer.ReleaseSurface();
            break;
        default:
            break;
    }
}

void RunApp(android_app* app) {
    App appData;
    app->userData = &appData;
    app->onAppCmd = HandleAppCmd;
    app->onInputEvent = HandleInput;
    appData.android = app;
    appData.lastFrame = std::chrono::steady_clock::now();

    // Sanity-check the lobby rule math once at startup.
    const auto assignment = AssignCharacters(2);
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "%s (%zu characters)",
                        assignment.valid ? "Rules ready"
                                         : assignment.error.c_str(),
                        assignment.characters.size());

    while (true) {
        int events = 0;
        android_poll_source* source = nullptr;
        while (ALooper_pollOnce(0, nullptr, &events,
                                reinterpret_cast<void**>(&source)) >= 0) {
            if (source != nullptr) source->process(app, source);
            if (app->destroyRequested) {
                appData.net.Close();
                appData.renderer.Shutdown();
                return;
            }
        }

        const auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - appData.lastFrame).count();
        appData.lastFrame = now;
        if (dt < 0.0f) dt = 0.0f;
        if (dt > 0.1f) dt = 0.1f;
        appData.uiTime += dt;

        if (appData.isHost) {
            HostTick(appData, dt);
        } else if (appData.net.IsOpen()) {
            ClientTick(appData, dt);
        }

        if (app->window != nullptr) {
            if (!appData.renderer.HasSurface()) appData.renderer.Prepare(app);
            if (appData.renderer.HasSurface()) {
                appData.renderer.BeginFrame();
                if (appData.screen == Screen::kMatch) {
                    DrawMatch(appData);
                } else {
                    DrawLobby(appData);
                }
                appData.renderer.EndFrame();
            }
        }
    }
}

}  // namespace

}  // namespace soccer

// Global entry point expected by android_native_app_glue (which the CMake
// build compiles as C++ into this same library): forwards to the namespaced
// runner above. Must stay outside every namespace.
void android_main(android_app* app) { soccer::RunApp(app); }








