// Recherche sur le graphe d'etats du duel.
//
// L'arbre d'actions n'est enumerable a aucune vitesse (10^97 le long de la
// seule ligne de reference). Ce qui rend l'exploration possible, c'est de
// chercher sur le GRAPHE D'ETATS : activer A puis B et B puis A convergent sur
// le meme noeud, et la table de transposition les fusionne.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "duel.h"
#include "enumerate.h"
#include "prompt.h"
#include "replay.h"

namespace solver {

// Board cible au sens du critere d'equivalence retenu : memes cartes par TYPE
// de zone, memes positions, memes materiaux, memes compteurs. La colonne exacte
// est ignoree.
struct BoardKey {
	uint64_t hash = 0;
	std::vector<uint64_t> entries;   // triees, pour une comparaison exacte
	// Codes seuls, tries. Beaucoup plus grossier que `entries`, mais c'est
	// justement ce qu'il faut pour guider : une carte posee compte des qu'elle
	// est la, sans attendre d'avoir ses materiaux et sa position finale.
	std::vector<uint32_t> codes;
	bool operator==(const BoardKey& o) const { return entries == o.entries; }
};

BoardKey ComputeBoardKey(Duel& duel, uint8_t con);

// Empreinte d'etat servant de cle de transposition. Les zones donnent le
// visible ; la charge utile du prompt donne l'essentiel de l'invisible — quels
// effets sont encore activables, donc les compteurs "une fois par tour" que
// l'API publique n'expose pas.
//
// Sous-hacher fusionne des etats distincts et fait DISPARAITRE des solutions
// sans le signaler : c'est le mode de defaillance a surveiller.
uint64_t StateDigest(Duel& duel, uint8_t prompt_type,
					 const std::vector<uint8_t>& prompt_payload);

// Une etape de la ligne de reference, exprimee en CARTES et non en indices.
//
// Une reponse enregistree dit "le troisieme element de la liste". Dans un autre
// duel la liste n'a ni le meme contenu ni le meme ordre : rejouer les octets ne
// veut rien dire. L'arete, elle, est batie sur les codes de cartes et sur la
// nature du choix — elle survit au changement de deck, de main et de graine.
struct PlanStep {
	uint8_t prompt_type = 0;
	uint64_t edge = 0;     // 0 = etape non identifiee, inutilisable comme guide
	std::string label;
};

// Releve la ligne de reference sous forme semantique.
//
// La reponse enregistree n'est pas comparable octet par octet a ce qu'enumere
// le solveur (EDOPro encode ses selections en bitset, l'enumerateur en liste
// d'index). On identifie donc chaque decision par l'ETAT qu'elle atteint :
// appliquer, comparer, restaurer — le meme test que la verification de
// couverture, que l'arene rend abordable.
//
// Le duel doit etre au depart ; il est laisse en fin de ligne. Renvoie le
// nombre d'etapes non identifiees, qui sont autant de trous dans le guide.
size_t LiftPlan(Duel& duel, Arena& arena, const Replay& yrp, int target_player,
				size_t stop_after, const EnumOptions& eo,
				std::vector<PlanStep>& out);

struct SearchConfig {
	int target_player = 0;
	uint32_t max_decisions = 24;      // profondeur, en decisions
	uint32_t max_actions = 0;         // 0 = pas de borne (sinon A_ref)
	double time_limit_ms = 30000;
	uint64_t max_nodes = 2000000;
	EnumOptions enumeration;
	bool collect_solutions = true;
	size_t max_solutions = 64;

	// Partition du travail entre workers. Le sous-arbre ouvert par la PREMIERE
	// deviation est independant de tous les autres, ce qui permet de partager
	// sans aucune synchronisation pendant l'exploration.
	//
	// L'attribution est DYNAMIQUE : un partage statique serait tres desequilibre,
	// car devier tot ouvre un sous-arbre enorme et devier tard un sous-arbre
	// minuscule. Chaque worker parcourt l'echine de reference et reclame les
	// points encore libres ; les workers rapides en prennent davantage.
	// Niveau d'ecart auquel se fait la reclamation. Reclamer le PREMIER ecart
	// serait desequilibre : le travail se concentre dans quelques sous-arbres
	// precoces, et les workers qui n'en attrapent pas terminent aussitot. En
	// reclamant au deuxieme, tous les workers entrent dans les gros sous-arbres
	// et s'y partagent le travail.
	uint32_t claim_level = 1;
	std::atomic<uint32_t>* claims = nullptr;
	size_t claims_size = 0;

	// Transplantation : ordre de preference entre deux coups du repertoire.
	// Sert uniquement a visiter d'abord ceux que la reference jouait tot.
	uint32_t plan_window = 32;
	// Trace la descente le long du plan : a chaque prompt, ce qui etait
	// propose et si le plan s'y retrouvait. Sert a voir OU un deck decroche.
	bool trace = false;
};

struct Solution {
	std::vector<std::vector<uint8_t>> responses;
	uint32_t actions = 0;      // invocations + activations (tier 2)
	uint32_t decisions = 0;    // tier 3
	uint32_t burned = 0;       // cartes au cimetiere + bannies (tier 1)
	uint32_t hand_left = 0, deck_left = 0, extra_left = 0;
};

struct SearchStats {
	uint64_t nodes = 0;             // etats developpes
	uint64_t transpositions = 0;    // fusions par la table
	uint64_t dead_ends = 0;         // reponses rejetees par le core
	uint64_t terminals = 0;
	uint64_t edges_skipped = 0;
	// Nombre d'etats DISTINCTS atteints a chaque profondeur : c'est la courbe
	// qui decide si "exhaustif" est un mot realiste.
	std::vector<uint64_t> distinct_by_depth;
	std::vector<uint64_t> expansions_by_depth;
	// Meilleure approche du board cible rencontree : nombre de cartes cibles
	// reunies en meme temps, et nombre de monstres poses. Quand la recherche ne
	// trouve rien, c'est ce couple qui distingue "il faut chercher plus loin" de
	// "ce deck ne peut pas enchainer".
	uint32_t best_overlap = 0;
	uint32_t best_monsters = 0;
	// Le board effectivement obtenu au moment de la meilleure approche. Sans
	// lui, "5 des 8 cartes" ne dit pas LESQUELLES manquent — c'est pourtant la
	// seule information sur laquelle on puisse agir.
	std::vector<uint32_t> best_board;
	double ms = 0;
	bool exhausted = false;         // espace epuise dans les bornes donnees
	bool hit_time_limit = false;
	bool hit_node_limit = false;
};

class Search {
public:
	Search(Duel& duel, Arena& arena, const Replay& yrp, const SearchConfig& cfg);

	// Enumeration exhaustive bornee en profondeur. Le duel doit etre positionne
	// au point de depart de la recherche.
	void Run(const BoardKey& target);

	// Recherche GUIDEE vers le board cible.
	//
	// L'exhaustif ne depasse pas ~50 decisions alors que la ligne de reference
	// en compte 276 : atteindre le board demande d'orienter la descente. A
	// chaque noeud on evalue les fils (avancer / mesurer / restaurer, ce que
	// l'instantane rend abordable) et on descend d'abord vers celui qui place
	// le plus de cartes du board cible.
	void RunGuided(const BoardKey& target);

	// Recherche a ECARTS BORNES, amorcee sur la ligne de reference.
	//
	// C'est la strategie adaptee au probleme pose : une meilleure ligne pour le
	// MEME board est presque surement une petite perturbation de la ligne
	// connue — ne pas activer une carte, prendre un autre materiau. On suit
	// donc la reference et on s'autorise au plus `discrepancies` deviations,
	// en tentant de reprendre la ligne enregistree apres chacune.
	//
	// Propriete utile : a zero ecart, la recherche rejoue la reference et la
	// retrouve donc forcement. Le solveur ne peut plus rendre "aucune solution"
	// sans que ce soit un defaut.
	void RunRepair(const BoardKey& target, uint32_t discrepancies);

	// TRANSPLANTATION : atteindre le meme board depuis un AUTRE duel — autre
	// deck, autre main, autre graine.
	//
	// Ici la ligne de reference n'existe plus : ses reponses ne designent rien
	// dans ce duel. Ce qui subsiste, c'est son INTENTION, relevee par LiftPlan.
	// Le plan est traite comme un REPERTOIRE, pas comme un calendrier. Deux
	// decks ne posent pas les memes questions dans le meme ordre : exiger un
	// alignement sequentiel fait decrocher des la premiere question inedite —
	// c'est mesure, pas suppose. Un coup que la reference a joue, n'importe ou
	// dans sa ligne, est donc gratuit ; tout autre coup coute un ecart. Le
	// budget mesure alors ce qu'il faut INVENTER en plus du repertoire.
	//
	// Contrairement a RunRepair, rien ne garantit qu'une solution existe : un
	// deck peut simplement ne pas avoir les cartes du board.
	void RunTransplant(const BoardKey& target, const std::vector<PlanStep>& plan,
					   uint32_t discrepancies);

	// TIRAGES GLOUTONS. La recherche a ecarts bornes est large et courte :
	// elle epuise le repertoire a zero ecart sans jamais s'approcher du board,
	// puis explose des le premier coup invente. Or le board est a ~300
	// decisions — ce qu'il faut, c'est de la PROFONDEUR.
	//
	// Un tirage descend d'un trait jusqu'au bout du tour, en choisissant a
	// chaque pas parmi les meilleurs fils selon l'heuristique, avec une part
	// d'alea qui varie d'un tirage a l'autre. Mille tirages visitent mille
	// lignes profondes distinctes la ou la descente en profondeur d'abord
	// s'enferme dans un seul sous-arbre. Le repertoire sert de prior : un coup
	// que la reference a joue part avec une prime.
	void RunRollouts(const BoardKey& target, const std::vector<PlanStep>& plan,
					 uint32_t count, uint64_t seed);

	const SearchStats& Stats() const { return stats; }
	const std::vector<Solution>& Solutions() const { return solutions; }

private:
	enum class Step { Prompt, Ended, Rejected };

	Step StepToPrompt();
	uint64_t Digest() const;
	void Descend(uint32_t depth, uint32_t actions);
	bool DescendGuided(uint32_t depth, uint32_t actions);
	bool DescendRepair(uint32_t depth, uint32_t actions, size_t ref_index,
					   uint32_t disc);
	bool DescendTransplant(uint32_t depth, uint32_t actions, uint32_t disc);
	// Un tirage, de la position courante jusqu'a la fin du tour. Renvoie true
	// si le board cible a ete atteint.
	bool Rollout(uint64_t& rng);
	// Nombre d'entrees du board cible deja en place : distance au but.
	uint32_t Heuristic() const;
	bool BudgetExhausted() const;

	Duel& duel;
	Arena& arena;
	const Replay& yrp;
	SearchConfig cfg;

	// Etat du prompt courant
	uint8_t prompt_type = 0;
	std::vector<uint8_t> prompt_payload;
	int prompt_player = -1;
	uint32_t actions_this_step = 0;
	bool saw_retry = false;
	bool ended = false;

	// Table de transposition. Indexee par etat, avec le budget restant sous
	// lequel l'etat a ete resolu : "explore avec 8 decisions restantes"
	// n'autorise pas a elaguer quand on arrive avec 12.
	std::unordered_map<uint64_t, uint32_t> tt;

	BoardKey target;
	uint32_t cfg_discrepancies = 0;   // budget d'ecarts du passage en cours
	const std::vector<PlanStep>* plan = nullptr;
	// Repertoire des coups de la reference : identite semantique -> rang de sa
	// premiere apparition dans la ligne, qui sert d'ordre de visite.
	std::unordered_map<uint64_t, size_t> plan_index;
	std::vector<Solution> solutions;
	std::vector<std::vector<uint8_t>> path;
	SearchStats stats;
	std::chrono::steady_clock::time_point start;
};

} // namespace solver
