// Enumeration des reponses legales a un MSG_SELECT_*.
//
// Chaque message porte deja la liste des choix legaux : on n'a aucune regle du
// jeu a reimplementer, seulement a decoder puis a re-encoder au format attendu
// par OCG_DuelSetResponse (ProgressiveBuffer, cf. progressivebuffer.h).
//
// Les classes d'equivalence appliquees ici sont le second levier de reduction
// apres la table de transposition — et le plus risque : chacune est une
// hypothese. Toutes sont desactivables, et toutes sont rattrapees par la
// verification finale, qui rejoue chaque candidat depuis zero.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "assets.h"

namespace solver {

struct Choice {
	std::vector<uint8_t> response;
	// Identifiant stable de l'arete, independant de l'etat atteint. Permet a la
	// table de transposition d'etre indexee par (etat parent, arete) et donc de
	// sauter un sous-arbre deja pris SANS avoir a le materialiser.
	uint64_t edge = 0;
	std::string label;
	// Identite du choix pour l'appariement a un plan releve sur un AUTRE duel.
	// Vaut `edge` par defaut ; ne s'en ecarte que la ou l'equivalence de board
	// ignore deja le detail, c'est-a-dire le choix de colonne. Deux placements
	// dans deux colonnes libres distinctes menent a des etats differents
	// (l'arete doit les separer) mais realisent la meme intention (le plan doit
	// les confondre). En dernier : le reste du code construit des Choice par
	// agregat a trois membres.
	uint64_t plan_key = 0;
};

struct EnumOptions {
	// Fusionne les choix portant sur des cartes de meme code au meme endroit.
	bool dedup_by_code = true;
	// Plafonne la taille des sous-ensembles enumeres (SELECT_CARD, SUM...).
	// Au-dela, on echantillonne les extremes plutot que d'exploser.
	uint32_t max_subsets = 64;
	// N'explore qu'une zone libre representative par type de zone. Faux par
	// defaut : les fleches de lien et les colonnes peuvent tout changer.
	bool canonical_zones = false;
	// Explorer le passage en Battle Phase / End Phase depuis l'idle command.
	bool allow_phase_change = true;
	// Table des alias, pour confondre les variantes d'illustration. Sans elle,
	// deux exemplaires du meme Quetzacoatl (29053656 et 29053657) produisent des
	// aretes differentes : une ligne relevee sur un deck ne se reconnaitrait pas
	// dans l'autre, et la deduplication laisserait passer des doublons.
	const CardDB* db = nullptr;
};

// Renvoie la liste des reponses legales. Vide si le message n'est pas un prompt
// ou si son espace de reponses n'est pas enumerable de facon sure (ANNOUNCE_*).
std::vector<Choice> Enumerate(uint8_t message, const uint8_t* data, uint32_t len,
							  const EnumOptions& opt);

// Reponse par defaut acceptable pour les prompts non enumerables, afin que la
// partie puisse continuer au lieu de partir en MSG_RETRY.
bool DefaultResponse(uint8_t message, const uint8_t* data, uint32_t len,
					 std::vector<uint8_t>& out);

} // namespace solver
