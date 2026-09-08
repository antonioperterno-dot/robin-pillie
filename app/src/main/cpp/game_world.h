#pragma once

#include <cstdint>

#include "game_rules.h"

namespace soccer {

// Pocket Soccer: shared match simulation. The host phone runs MatchSim and
// rebroadcasts state; clients only render snapshots. Units are meters and
// seconds; the pitch is 105 x 68 with the origin at its center.
//
// Coordinate system: team 0 defends the left goal (x = -halfW) and attacks
// to the right (+x). Team 1 mirrors that.
constexpr float kFieldHalfW = 52.5f;
constexpr float kFieldHalfH = 34.0f;
constexpr float kGoalHalfW = 9.5f;
constexpr float kCharRadius = 1.8f;
constexpr float kBallRadius = 0.8f;
constexpr float kMatchDuration = 90.0f;  // short arcade match
constexpr float kControlledSpeed = 13.0f;

enum class MatchPhase : uint8_t {
    kKickoff = 0,
    kPlay = 1,
    kGoal = 2,
    kFullTime = 3,
};

// Input for one phone, updated every tick by the host (local touch for
// phone 0) and by input packets for the rest.
struct PhoneInput {
    float dx = 0.0f;
    float dy = 0.0f;
    bool kick = false;
};

struct MatchChar {
    float x = 0.0f;
    float y = 0.0f;
    float vx = 0.0f;
    float vy = 0.0f;
    int team = 0;
};

// Plain-old-data on purpose: the whole struct is what the host sends to
// clients over UDP (encoded field-by-field by main.cpp).
struct MatchState {
    MatchChar chars[kTeamSize * 2];
    float ballX = 0.0f;
    float ballY = 0.0f;
    float ballVx = 0.0f;
    float ballVy = 0.0f;
    uint8_t score[2] = {0, 0};
    MatchPhase phase = MatchPhase::kKickoff;
    float phaseTimer = 0.0f;
    float timeLeft = kMatchDuration;
    int activeChar[kMaxPhones] = {0};  // active char per phone (rendered)
    int phoneCount = 2;
};

class MatchSim {
public:
    void Start(int phoneCount);
    // inputs must have kMaxPhones entries; only the first
    // state_.phoneCount are read.
    void Step(float dt, const PhoneInput* inputs);
    const MatchState& state() const { return state_; }

private:
    void ResetPositions();  // back to kickoff formation
    void UpdateActive();    // nearest-to-ball switching with hysteresis
    void GoalScored(int scoringTeam);
    void ClampToField(MatchChar& c);
    int OwnerPhone(int charId) const;  // phone controlling char, else -1

    MatchState state_;
    int charsForPhone_[kMaxPhones][kTeamSize] = {{0}};
    int charsForPhoneCount_[kMaxPhones] = {0};
    float homeX_[kTeamSize * 2] = {0.0f};
    float homeY_[kTeamSize * 2] = {0.0f};
    float kickCd_[kMaxPhones] = {0.0f};
};

}  // namespace soccer
