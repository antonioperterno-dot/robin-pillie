#include "game_rules.h"

namespace soccer {

bool IsSupportedPhoneCount(int phoneCount) {
    return phoneCount >= kMinPhones && phoneCount <= kMaxPhones && phoneCount % 2 == 0;
}

AssignmentResult AssignCharacters(int phoneCount) {
    AssignmentResult result{true, {}, {}};
    if (!IsSupportedPhoneCount(phoneCount)) {
        result.valid = false;
        result.error = "Phone count must be an even number from 2 through 22.";
        return result;
    }

    const int phonesPerTeam = phoneCount / 2;
    result.characters.reserve(kTeamSize * 2);
    for (int characterId = 0; characterId < kTeamSize * 2; ++characterId) {
        const int team = characterId < kTeamSize ? 0 : 1;
        const int teamCharacterId = characterId % kTeamSize;
        result.characters.push_back({
            characterId,
            team,
            team * phonesPerTeam + teamCharacterId % phonesPerTeam
        });
    }
    return result;
}

}  // namespace soccer
