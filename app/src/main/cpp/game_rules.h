#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace soccer {
constexpr int kTeamSize = 11;
constexpr int kMaxPhones = 22;
constexpr int kMinPhones = 2;

struct CharacterAssignment {
    int characterId;
    int team;
    int phoneId;
};

struct AssignmentResult {
    bool valid;
    std::string error;
    std::vector<CharacterAssignment> characters;
};

bool IsSupportedPhoneCount(int phoneCount);
AssignmentResult AssignCharacters(int phoneCount);

}  // namespace soccer
