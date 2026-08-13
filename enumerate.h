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
#include <map>
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
	// les confondre).
	uint64_t plan_key = 0;
	// Carte canonique que ce choix ENGAGE (invoquer, activer, attaquer,
	// positionner) — 0 si le choix n'en engage aucune. Support des indices de
	// domaine (--hint) : "cette carte-la, essaie-la plus souvent".
	uint32_t card = 0;
	// Changement de phase ("-> Battle Phase", "-> End Phase", "-> Main 2").
	// Porte par un drapeau et non par le label : les chemins chauds n'ont pas
	// de label, et le biais GNRPA doit continuer a exclure ces coups (finir le
	// tour est irreversible).
	bool phase = false;
};

// Pool de Choice reutilisable entre les decisions. Un tirage NRPA enumere a
// CHAQUE decision ; reconstruire vecteurs et chaines des dizaines de millions
// de fois par run etait le premier point chaud mesure. Ici, Clear() ne detruit
// rien : les reponses et labels des elements gardent leur capacite, et Emit()
// les reutilise.
class ChoiceList {
public:
	Choice& Emit() {
		if(used == store.size())
			store.emplace_back();
		Choice& c = store[used++];
		c.response.clear();
		c.label.clear();
		c.edge = 0;
		c.plan_key = 0;
		c.card = 0;
		c.phase = false;
		return c;
	}
	void Clear() { used = 0; }
	size_t size() const { return used; }
	bool empty() const { return used == 0; }
	Choice& operator[](size_t i) { return store[i]; }
	const Choice& operator[](size_t i) const { return store[i]; }
	Choice* begin() { return store.data(); }
	Choice* end() { return store.data() + used; }
	const Choice* begin() const { return store.data(); }
	const Choice* end() const { return store.data() + used; }
	const Choice& back() const { return store[used - 1]; }
	// Reduit la liste au seul dernier element ("ne rien faire" de l'adversaire)
	// par echange de tampons : aucune copie de contenu.
	void KeepOnlyLast() {
		if(used > 1) {
			std::swap(store[0], store[used - 1]);
			used = 1;
		}
	}

private:
	std::vector<Choice> store;
	size_t used = 0;
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
	// Construire les labels humains. Les chemins chauds (tirages, LDS) n'en
	// ont pas besoin : chaque label est une allocation de chaine par choix.
	bool labels = false;
	// Table des alias, pour confondre les variantes d'illustration. Sans elle,
	// deux exemplaires du meme Quetzacoatl (29053656 et 29053657) produisent des
	// aretes differentes : une ligne relevee sur un deck ne se reconnaitrait pas
	// dans l'autre, et la deduplication laisserait passer des doublons.
	const CardDB* db = nullptr;
	// Activations interdites : code canonique -> masque de LOCATION_ depuis
	// lesquelles la carte ne doit pas etre activee. Le choix n'est simplement
	// jamais emis — la contrainte retire la branche de l'espace au lieu de la
	// rejeter apres coup. Ne couvre que les commandes idle : c'est la que
	// vivent les effets ignes "superflus" (payer 2000 LP...) qu'on veut bannir,
	// et interdire une fenetre de chaine changerait la legalite du jeu.
	const std::map<uint32_t, uint32_t>* no_activate = nullptr;
	// Cartes jamais CHAINEES par le joueur (codes canoniques, --no-chain).
	// Les gardes (Zalen, Crystal Wing) existent pour repondre a Nibiru — une
	// menace HYPOTHETIQUE tenue par la garde ; en solitaire, toute fenetre de
	// chaine repond a nos PROPRES actions, et chainer leur negation sur nos
	// propres activations ne fait que multiplier des branches inutiles.
	// « Ne pas chainer » reste toujours propose, les declenchements FORCES
	// (chaines obligatoires) sont exempts, les commandes idle aussi.
	const std::vector<uint32_t>* no_chain = nullptr;
};

// Remplit `out` (reutilise, cf. ChoiceList) avec les reponses legales. Vide si
// le message n'est pas un prompt ou si son espace de reponses n'est pas
// enumerable de facon sure (ANNOUNCE_*).
void EnumerateInto(uint8_t message, const uint8_t* data, uint32_t len,
				   const EnumOptions& opt, ChoiceList& out);

// Variante allouante, pour les chemins froids (releve de plan, controle de
// couverture) : labels toujours construits.
std::vector<Choice> Enumerate(uint8_t message, const uint8_t* data, uint32_t len,
							  const EnumOptions& opt);

// Reponse par defaut acceptable pour les prompts non enumerables, afin que la
// partie puisse continuer au lieu de partir en MSG_RETRY.
bool DefaultResponse(uint8_t message, const uint8_t* data, uint32_t len,
					 std::vector<uint8_t>& out);

// Une reponse ENREGISTREE contourne l'enumerateur, donc son filtre
// d'activations interdites : ce test la rattrape (mode reparation, ou la
// reponse de la reference est candidate a cout zero sans etre enumeree).
bool ResponseForbidden(uint8_t message, const uint8_t* data, uint32_t len,
					   const std::vector<uint8_t>& response,
					   const EnumOptions& opt);

} // namespace solver
