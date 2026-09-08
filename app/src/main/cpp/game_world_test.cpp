#include "game_rules.h"
#include "game_world.h"

#include <cassert>
#include <cmath>

// Portable host-side sanity test: run a full 2-phone match with phone 0
// pushing upfield, then check bounds, active chars, and the final whistle.
// (Build: g++ -std=c++17 game_rules.cpp game_world.cpp game_world_test.cpp)
int main() {
    soccer::MatchSim sim;
    sim.Start(2);

    soccer::PhoneInput inputs[soccer::kMaxPhones] = {};
    inputs[0].dx = 1.0f;  // phone 0 runs right all match

    const auto& kickoff = sim.state();
    assert(kickoff.phase == soccer::MatchPhase::kKickoff);
    assert(kickoff.phoneCount == 2);

    // 240 s of steps: the 90 s clock pauses during kickoff/goal resets.
    for (int i = 0; i < 60 * 240; ++i) {
        sim.Step(1.0f / 60.0f, inputs);
        const auto& s = sim.state();
        for (int c = 0; c < soccer::kTeamSize * 2; ++c) {
            assert(std::fabs(s.chars[c].x) <= soccer::kFieldHalfW + 2.5f);
            assert(std::fabs(s.chars[c].y) <= soccer::kFieldHalfH + 1.5f);
        }
        assert(std::fabs(s.ballX) <= soccer::kFieldHalfW + 2.0f);
        assert(std::fabs(s.ballY) <= soccer::kFieldHalfH + 2.0f);
        if (s.phase == soccer::MatchPhase::kFullTime) break;
    }

    const auto& end = sim.state();
    assert(end.phase == soccer::MatchPhase::kFullTime);
    assert(end.timeLeft == 0.0f);
    for (int p = 0; p < end.phoneCount; ++p) {
        assert(end.activeChar[p] >= 0 &&
               end.activeChar[p] < soccer::kTeamSize * 2);
    }

    // Odd counts collapse to the even number below.
    sim.Start(7);
    assert(sim.state().phoneCount == 6);
    return 0;
}
