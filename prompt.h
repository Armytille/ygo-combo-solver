// Decodage des MSG_SELECT_* : ce sont les points de branchement du solveur.
//
// Chaque message porte deja la liste des choix legaux, on n'a donc aucune regle
// de jeu a reimplementer (docs/combo-solver-design.md 2.3).
#pragma once

#include <cstdint>
#include <string>

namespace solver {

// -1 = message non reconnu ou tronque.
struct PromptInfo {
	uint8_t type{};
	int player{ -1 };
	long double raw{ -1 };    // nombre de reponses distinctes offertes
	long double dedup{ -1 };  // apres fusion des choix equivalents par code
	std::string detail;
};

bool IsPrompt(uint8_t message);
const char* PromptName(uint8_t message);
PromptInfo DecodePrompt(uint8_t message, const uint8_t* data, uint32_t len);

} // namespace solver
