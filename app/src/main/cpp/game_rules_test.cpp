#include "game_rules.h"

#include <cassert>

int main() {
    for (int phoneCount = 2; phoneCount <= 22; phoneCount += 2) {
        const auto result = soccer::AssignCharacters(phoneCount);
        assert(result.valid);
        assert(result.characters.size() == 22);
        for (const auto& character : result.characters) {
            assert(character.phoneId >= 0 && character.phoneId < phoneCount);
            assert(character.team == 0 || character.team == 1);
        }
    }
    assert(!soccer::AssignCharacters(1).valid);
    assert(!soccer::AssignCharacters(23).valid);
    assert(!soccer::AssignCharacters(3).valid);
    return 0;
}
