#include "game_world.h"

#include <algorithm>
#include <cmath>

namespace soccer {
namespace {

float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
float Len(float x, float y) { return std::sqrt(x * x + y * y); }

}  // namespace

void MatchSim::Start(int phoneCount) {
    if (phoneCount < kMinPhones) {
        phoneCount = kMinPhones;
    } else if (phoneCount > kMaxPhones) {
        phoneCount = kMaxPhones;
    }
    if (phoneCount % 2 != 0) phoneCount -= 1;  // even teams only
    state_.phoneCount = phoneCount;

    const auto assignment = AssignCharacters(state_.phoneCount);
    for (int p = 0; p < kMaxPhones; ++p) {
        charsForPhoneCount_[p] = 0;
        for (int c = 0; c < kTeamSize; ++c) charsForPhone_[p][c] = -1;
    }
    for (const auto& ch : assignment.characters) {
        const int p = ch.phoneId;
        charsForPhone_[p][charsForPhoneCount_[p]++] = ch.characterId;
    }

    // Home formation fractions: GK, 4 DEF, 3 MID, 3 FWD.
    static const float kDefY[4] = {-24.0f, -8.0f, 8.0f, 24.0f};
    static const float kMidY[3] = {-14.0f, 0.0f, 14.0f};
    static const float kFwdY[3] = {-15.0f, 0.0f, 15.0f};
    for (int team = 0; team < 2; ++team) {
        const float dir = team == 0 ? 1.0f : -1.0f;
        const int base = team * kTeamSize;
        homeX_[base + 0] = dir * (0.06f * 2.0f * kFieldHalfW - kFieldHalfW);
        homeY_[base + 0] = 0.0f;
        for (int i = 0; i < 4; ++i) {
            homeX_[base + 1 + i] = dir * (0.24f * 2.0f * kFieldHalfW - kFieldHalfW);
            homeY_[base + 1 + i] = kDefY[i];
        }
        for (int i = 0; i < 3; ++i) {
            homeX_[base + 5 + i] = dir * (0.52f * 2.0f * kFieldHalfW - kFieldHalfW);
            homeY_[base + 5 + i] = kMidY[i];
        }
        for (int i = 0; i < 3; ++i) {
            homeX_[base + 8 + i] = dir * (0.74f * 2.0f * kFieldHalfW - kFieldHalfW);
            homeY_[base + 8 + i] = kFwdY[i];
        }
    }

    state_.score[0] = 0;
    state_.score[1] = 0;
    state_.timeLeft = kMatchDuration;
    for (int p = 0; p < kMaxPhones; ++p) {
        kickCd_[p] = 0.0f;
        state_.activeChar[p] = -1;
    }
    ResetPositions();
}

void MatchSim::ResetPositions() {
    for (int i = 0; i < kTeamSize * 2; ++i) {
        state_.chars[i].x = homeX_[i];
        state_.chars[i].y = homeY_[i];
        state_.chars[i].vx = 0.0f;
        state_.chars[i].vy = 0.0f;
        state_.chars[i].team = i < kTeamSize ? 0 : 1;
    }
    state_.ballX = 0.0f;
    state_.ballY = 0.0f;
    state_.ballVx = 0.0f;
    state_.ballVy = 0.0f;
    state_.phase = MatchPhase::kKickoff;
    state_.phaseTimer = 2.0f;
}
void MatchSim::UpdateActive() {
    // Each phone "possesses" its nearest-to-ball character; holding on to
    // the current one a little longer avoids rapid flicker.
    for (int p = 0; p < state_.phoneCount; ++p) {
        int best = -1;
        float bestD = 1e9f;
        for (int k = 0; k < charsForPhoneCount_[p]; ++k) {
            const int id = charsForPhone_[p][k];
            const float d = Len(state_.chars[id].x - state_.ballX,
                                state_.chars[id].y - state_.ballY);
            if (d < bestD) {
                bestD = d;
                best = id;
            }
        }
        const int cur = state_.activeChar[p];
        bool curValid = false;
        float curD = 1e9f;
        for (int k = 0; k < charsForPhoneCount_[p]; ++k) {
            if (charsForPhone_[p][k] == cur) {
                curValid = true;
                curD = Len(state_.chars[cur].x - state_.ballX,
                           state_.chars[cur].y - state_.ballY);
                break;
            }
        }
        state_.activeChar[p] = (curValid && curD * 0.75f < bestD) ? cur : best;
    }
}

int MatchSim::OwnerPhone(int charId) const {
    for (int p = 0; p < state_.phoneCount; ++p) {
        if (state_.activeChar[p] == charId) return p;
    }
    return -1;
}

void MatchSim::Step(float dt, const PhoneInput* inputs) {
    state_.phaseTimer -= dt;

    if (state_.phase == MatchPhase::kKickoff) {
        if (state_.phaseTimer <= 0.0f) state_.phase = MatchPhase::kPlay;
        UpdateActive();
        return;  // frozen during the countdown
    }
    if (state_.phase == MatchPhase::kGoal) {
        if (state_.phaseTimer <= 0.0f) ResetPositions();
        return;
    }
    if (state_.phase == MatchPhase::kFullTime) return;

    state_.timeLeft -= dt;
    if (state_.timeLeft <= 0.0f) {
        state_.timeLeft = 0.0f;
        state_.phase = MatchPhase::kFullTime;
        return;
    }

    UpdateActive();

    bool controlled[kTeamSize * 2] = {false};
    for (int p = 0; p < state_.phoneCount; ++p) {
        if (state_.activeChar[p] >= 0) controlled[state_.activeChar[p]] = true;
    }

    // Chaser per team: nearest non-keeper, non-controlled char hunts the ball.
    int chaser[2] = {-1, -1};
    float chaserD[2] = {1e9f, 1e9f};
    for (int i = 0; i < kTeamSize * 2; ++i) {
        if (i == 0 || i == kTeamSize) continue;  // keepers hold the line
        if (controlled[i]) continue;
        const float d = Len(state_.chars[i].x - state_.ballX,
                            state_.chars[i].y - state_.ballY);
        const int team = i < kTeamSize ? 0 : 1;
        if (d < chaserD[team]) {
            chaserD[team] = d;
            chaser[team] = i;
        }
    }

    for (int i = 0; i < kTeamSize * 2; ++i) {
        MatchChar& c = state_.chars[i];
        const int team = i < kTeamSize ? 0 : 1;
        if (controlled[i]) {
            const int p = OwnerPhone(i);
            c.vx = inputs[p].dx * kControlledSpeed;
            c.vy = inputs[p].dy * kControlledSpeed;
            c.x += c.vx * dt;
            c.y += c.vy * dt;
            ClampToField(c);
            continue;
        }
        float tx = c.x;
        float ty = c.y;
        float speed = 8.0f;
        if (i == 0 || i == kTeamSize) {  // keeper
            const float dir = team == 0 ? 1.0f : -1.0f;
            tx = dir * (kFieldHalfW - 5.5f);
            ty = Clamp(state_.ballY * 0.5f, -kGoalHalfW + 1.0f, kGoalHalfW - 1.0f);
        } else if (i == chaser[team]) {
            tx = state_.ballX;
            ty = state_.ballY;
            speed = 9.5f;
        } else {  // drift back + shadow the ball a little
            tx = homeX_[i];
            ty = homeY_[i] + (state_.ballY - homeY_[i]) * 0.25f;
        }
        const float dx = tx - c.x;
        const float dy = ty - c.y;
        const float d = Len(dx, dy);
        if (d > 0.4f) {
            c.vx = dx / d * speed;
            c.vy = dy / d * speed;
            c.x += c.vx * dt;
            c.y += c.vy * dt;
        } else {
            c.vx = 0.0f;
            c.vy = 0.0f;
        }
        ClampToField(c);
    }
    // Kicks: active char boots the ball when close (0.35 s per-phone cooldown).
    for (int p = 0; p < state_.phoneCount; ++p) {
        kickCd_[p] -= dt;
        const int id = state_.activeChar[p];
        if (id < 0 || !inputs[p].kick || kickCd_[p] > 0.0f) continue;
        const MatchChar& c = state_.chars[id];
        if (Len(c.x - state_.ballX, c.y - state_.ballY) > 4.0f) continue;
        float dx = inputs[p].dx;
        float dy = inputs[p].dy;
        if (Len(dx, dy) < 0.15f) {  // no aim: boot it upfield
            dx = c.x < 0.0f ? 1.0f : -1.0f;
            dy = 0.0f;
        } else {
            const float l = Len(dx, dy);
            dx /= l;
            dy /= l;
        }
        state_.ballVx = dx * 22.0f + c.vx * 0.4f;
        state_.ballVy = dy * 22.0f + c.vy * 0.4f;
        kickCd_[p] = 0.35f;
    }

    // Ball roll + player contact + pitch bounds + goals.
    const float drag = 1.0f - 0.55f * dt;
    state_.ballVx *= drag;
    state_.ballVy *= drag;
    state_.ballX += state_.ballVx * dt;
    state_.ballY += state_.ballVy * dt;

    if (state_.ballY > kFieldHalfH - kBallRadius) {
        state_.ballY = kFieldHalfH - kBallRadius;
        state_.ballVy = -state_.ballVy * 0.7f;
    }
    if (state_.ballY < -(kFieldHalfH - kBallRadius)) {
        state_.ballY = -(kFieldHalfH - kBallRadius);
        state_.ballVy = -state_.ballVy * 0.7f;
    }

    const bool inGoalMouth = std::fabs(state_.ballY) < kGoalHalfW;
    if (!inGoalMouth) {
        if (state_.ballX > kFieldHalfW - kBallRadius) {
            state_.ballX = kFieldHalfW - kBallRadius;
            state_.ballVx = -state_.ballVx * 0.7f;
        }
        if (state_.ballX < -(kFieldHalfW - kBallRadius)) {
            state_.ballX = -(kFieldHalfW - kBallRadius);
            state_.ballVx = -state_.ballVx * 0.7f;
        }
    } else if (state_.ballX > kFieldHalfW + kBallRadius) {
        GoalScored(0);  // team 0 attacks +x
        return;
    } else if (state_.ballX < -(kFieldHalfW + kBallRadius)) {
        GoalScored(1);
        return;
    }

    for (int i = 0; i < kTeamSize * 2; ++i) {
        const MatchChar& c = state_.chars[i];
        const float dx = state_.ballX - c.x;
        const float dy = state_.ballY - c.y;
        const float d = Len(dx, dy);
        const float minD = kCharRadius + kBallRadius;
        if (d < minD && d > 0.0001f) {
            state_.ballX = c.x + dx / d * minD;
            state_.ballY = c.y + dy / d * minD;
            state_.ballVx = dx / d * 13.0f + c.vx * 0.5f;
            state_.ballVy = dy / d * 13.0f + c.vy * 0.5f;
        }
    }
}

void MatchSim::GoalScored(int scoringTeam) {
    if (scoringTeam >= 0 && scoringTeam < 2) state_.score[scoringTeam]++;
    state_.phase = MatchPhase::kGoal;
    state_.phaseTimer = 2.0f;
    state_.ballX = 0.0f;
    state_.ballY = 0.0f;
    state_.ballVx = 0.0f;
    state_.ballVy = 0.0f;
}

void MatchSim::ClampToField(MatchChar& c) {
    c.x = Clamp(c.x, -kFieldHalfW - 2.0f, kFieldHalfW + 2.0f);
    c.y = Clamp(c.y, -kFieldHalfH - 1.0f, kFieldHalfH + 1.0f);
}

}  // namespace soccer


