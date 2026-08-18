// combosolver — jalon 0 : rejeu instrumente et validation de l'arene.
//
// Reproduit fidelement la ligne jouee dans un .yrpX, mesure son cout et le
// branchement offert par le core a chaque decision, capture le board cible,
// puis verifie que l'instantane memoire restaure un etat rigoureusement
// identique. Sans rejeu fidele, le board cible est faux ; sans restauration
// fidele, toute la recherche l'est aussi.

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "arena.h"
#include "assets.h"
#include "duel.h"
#include "enumerate.h"
#include "operators.h"
#include "prompt.h"
#include "replay.h"
#include "search.h"

using namespace solver;

namespace {

using Clock = std::chrono::steady_clock;

double MsSince(Clock::time_point t0) {
	return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

// Les six mecanismes qui SUPPRIMENT des branches, sur une ligne, sous chaque
// passe. Tous etaient actifs dans chaque run discipline depuis la session 3 et
// aucun n'etait chiffre (piege 52 : un compteur qui n'est pas imprime n'est pas
// un instrument).
//   contrainte = --summon-min / --material    garde     = --guard
//   tour       = ligne debordant du tour 1    borne     = plafond decisions/actions
//   partition  = branches cedees a un autre worker (ClaimTable)
//   sous-ens.  = enumerations tronquees par max_subsets
// `borne` et `sous-ens.` non nuls retirent a "EPUISE" sa valeur de preuve
// d'absence ; `partition` non nul dit que le travail a ete PARTAGE, pas
// SUPPRIME — c'est la distinction que la session 8 n'avait pas.
struct CutCounts {
	uint64_t constraint = 0, guard = 0, turn = 0, bound = 0, claim = 0,
			 subsets = 0, selfneg = 0;
	// --- ce qui n'est PAS un elagage, mais une amputation de l'espace ---
	uint64_t forced = 0;          // prompts reduits a la reponse par defaut
	uint64_t forced_mask = 0;     // quels types de prompts
	// DISTINCT DU PRECEDENT (audit 18) : ici la branche ne survit PAS. Le
	// compteur unique melangeait les deux et comptait la seconde famille deux
	// fois (elle tombe aussi dans `dead_ends`) — d'ou le « 90 forces / 90
	// impasses » de 9.24 (h), qui etait UN fait et non deux.
	uint64_t killed = 0;
	uint64_t killed_mask = 0;
	// --- sante de la recherche, jamais imprimee jusqu'ici ---
	uint64_t dead_ends = 0, terminals = 0;
	uint64_t novel = 0, stale = 0;   // taux de nouveaute des tirages
	size_t atoms = 0;                // largeur mesuree de la table d'atomes
	uint64_t num_broken = 0;         // arithmetique sqrt-LTS cassee
	// --- graphe de recettes (chantier 16) ---
	uint64_t recipes_seen = 0;       // invocations observees et versees
	double recipe_h_sum = 0.0;       // somme des distances evaluees
	uint64_t recipe_h_count = 0;
	// --- graphe de landmarks (chantier 18) ---
	double landmark_h_sum = 0.0;
	uint64_t landmark_h_count = 0;
	void Add(const SearchStats& s) {
		constraint += s.constraint_cuts;
		guard      += s.guard_cuts;
		selfneg    += s.self_negate_cuts;
		turn       += s.turn_cuts;
		bound      += s.edges_skipped;
		claim      += s.claim_denied;
		subsets    += s.subsets_capped;
		forced     += s.forced_default;
		forced_mask |= s.forced_default_prompts;
		killed     += s.forced_killed;
		killed_mask |= s.forced_killed_prompts;
		dead_ends  += s.dead_ends;
		terminals  += s.terminals;
		novel      += s.novelty_novel;
		stale      += s.novelty_stale;
		atoms       = (std::max)(atoms, s.novelty_atoms);
		num_broken += s.levin_overflow + s.lam_saturated;
		recipes_seen   += s.recipes_seen;
		recipe_h_sum   += s.recipe_h_sum;
		recipe_h_count += s.recipe_h_count;
		landmark_h_sum   += s.landmark_h_sum;
		landmark_h_count += s.landmark_h_count;
	}
};

// Un worker qui n'a pas pu s'initialiser retournait EN SILENCE. Sa passe
// affichait alors « 0 solutions, 0 etats » — exactement ce qu'affiche un worker
// qui a bien tourne et n'a rien trouve. Sous pression d'espace d'adressage
// (16 workers x --arena-mb) un bras d'A/B entier pouvait donc n'avoir jamais
// tourne sans que rien ne le dise. C'etait d'autant moins une politique que
// BuildPriorPolicy, lui, imprimait deja dans ce cas (C8).
void WorkerAbort(const char* ou, const std::string& err) {
	static std::mutex abort_mx;
	std::lock_guard<std::mutex> lk(abort_mx);
	std::printf("  !! worker %s : %s\n", ou,
				err.empty() ? "echec sans message" : err.c_str());
	std::fflush(stdout);
}

// Arene debordee dans un worker : ses mesures sont INVALIDES a partir du repli,
// pas seulement incompletes — `Restore()` ne restaure pas les objets partis sur
// le tas de l'hote, donc le duel diverge de ce que la recherche croit avoir
// restaure. Le compteur qui aurait du le dire etait un `thread_local` lu depuis
// le thread PRINCIPAL, donc structurellement nul quoi qu'il arrive : le rapport
// imprimait « aucune : tout l'etat est capture » par construction (C7).
void ReportPoison(const char* ou, const Arena& a) {
	if(!a.Poisoned())
		return;
	static std::mutex poison_mx;
	std::lock_guard<std::mutex> lk(poison_mx);
	std::printf("  !! ARENE CORROMPUE — worker %s : %zu allocation(s) hors "
				"arene.\n     Restore() ne les restaure pas : tout ce que ce "
				"worker a mesure ensuite est FAUX.\n     Augmenter --arena-mb, "
				"ou reduire --threads.\n", ou, a.Fallbacks());
	std::fflush(stdout);
}

// Distribution sous laquelle le RAPPORT DE POLITIQUE sonde le corpus. Elle est
// NEUTRE, et volontairement differente de celle du run (cfg.hint_bias = 2,
// cfg.nrpa_temp reglable par --nrpa-temp) : le rapport mesure le pouvoir
// discriminant du corpus lui-meme, et ses trois instruments — AdaptCorpus,
// CorpusAgreement, ForecastSearchCost — doivent au minimum s'accorder ENTRE EUX,
// ce que seuls les deux derniers imposaient. Les valeurs sont ecrites ici, une
// fois, au lieu d'etre omises a l'appel : c'est l'omission qui avait rendu
// l'incoherence invisible (C11).
//
// Consequence a garder en tete : les chiffres du §9.14 sont lus sous CETTE
// distribution, pas sous celle des tirages. Aligner les trois instruments sur
// le run est un chantier a part, qui re-mesure le §9.14.
constexpr float kReportHintBias = 0.0f;
constexpr float kReportTemp = 1.0f;

// SATURATIONS SILENCIEUSES (C13). Trois encodages compacts clampent leurs
// champs sans avertir, et deux d'entre eux gouvernent des grandeurs qui ont
// servi a decider : le score d'archive (rp sur 4 bits, overlap sur 8) ordonne
// les etats conserves, et ContextKey (15 max) borne la segmentation que
// ForecastSearchCost a lue pour ecrire sqrt-LTS. Verifie une fois, au demarrage,
// comme le fait deja le plafond de 4 entrees --resolve.
void CheckSaturations(size_t target_size,
					  const std::vector<ResolveReq>& resolve_min) {
	uint32_t total = 0;
	for(const ResolveReq& r : resolve_min)
		total += r.min_count;
	if(total > 15)
		std::printf("!! %u resolutions exigees : le score d'archive n'en encode "
					"que 15\n   (les etats au-dela sont classes a egalite — "
					"reduire --resolve)\n", total);
	if(target_size > 255)
		std::printf("!! board cible de %zu cartes : le score d'archive n'en "
					"encode que 255\n", target_size);
	if(target_size > 15)
		std::printf("!! board cible de %zu cartes : ContextKey n'en distingue "
					"que 15\n   (le contexte de politique et la segmentation de "
					"ForecastSearchCost saturent)\n", target_size);
}

// Profondeur restante pour le finisseur apres rejeu d'un prefixe.
//
// Quand le prefixe atteint deja le plafond (lui-meme derive de la reference),
// le reste est nul et le finisseur recevait 64 decisions — une valeur au jugé,
// ECRITE CINQ FOIS, et SILENCIEUSE. Sur un `--finisher ab` compare « a budget
// egal », les deux moteurs pouvaient donc recevoir des budgets de PROFONDEUR
// differents sans un mot dans le log (2.5). Le repli est desormais compte, et
// le bilan le dit — comme le fait deja celui de --max-decisions.
constexpr uint32_t kFinisherFallbackDepth = 64;
std::atomic<uint64_t> g_depth_fallbacks{ 0 };

uint32_t FinisherDepth(uint32_t ceiling, size_t prefix) {
	if(ceiling > prefix)
		return static_cast<uint32_t>(ceiling - prefix);
	g_depth_fallbacks.fetch_add(1, std::memory_order_relaxed);
	return kFinisherFallbackDepth;
}

// --- COMPTAGE DERIVE DU BOARD CIBLE (chantier 16, premier pas) ---------------
//
// Le board cible seul impose une ARITHMETIQUE, sans aucun modele declaratif :
// « 3x Liger Dancer » veut dire TROIS invocations Fusion. C'est un argument sur
// le multi-ensemble cible et la decklist, dans l'esprit du comptage
// d'operateurs, et il donne deux choses que le solveur ecrivait a la main :
// une borne de faisabilite plus fine, et un --summon-min DERIVE.
//
// LE PIEGE, et il est explicite dans la revue : on compte les EVENEMENTS
// d'invocation, JAMAIS leurs declencheurs. « Trois Liger donc trois
// Polymerisations » serait faux — Lunalight Wolf fusionne depuis la zone
// Pendule sans Polymerisation. Le type de la carte cible dit quel EVENEMENT
// doit se produire ; il ne dit rien de ce qui le declenche.
//
// ET LA REGLE 2 DU CHANTIER : ceci ne PRUNE jamais. Un manque de copies est
// rapporte comme un DOUTE, pas comme une impossibilite, parce qu'une carte qui
// copie un nom (Kaleido Chick prenant le nom de Leo Dancer) satisfait un but
// fonde sur le code effectif sans etre une copie physique. Servir d'oracle ici
// supprimerait des solutions en silence — la forme exacte du piege 47.

constexpr uint32_t kTypeMonster  = 0x1;
constexpr uint32_t kTypeFusion   = 0x40;
constexpr uint32_t kTypeRitual   = 0x80;
constexpr uint32_t kTypeSynchro  = 0x2000;
constexpr uint32_t kTypeToken    = 0x4000;
constexpr uint32_t kTypeXyz      = 0x800000;
constexpr uint32_t kTypePendulum = 0x1000000;
constexpr uint32_t kTypeLink     = 0x4000000;

// Mecanisme de mise en jeu impose par le TYPE de la carte cible.
enum class Mech { Fusion, Synchro, Xyz, Link, Ritual, MainMonster, SpellTrap };

const char* MechName(Mech m) {
	switch(m) {
	case Mech::Fusion:      return "Fusion";
	case Mech::Synchro:     return "Synchro";
	case Mech::Xyz:         return "Xyz";
	case Mech::Link:        return "Lien";
	case Mech::Ritual:      return "Rituelle";
	case Mech::MainMonster: return "mise en jeu (main deck)";
	default:                return "pose/activation";
	}
}

Mech MechOf(uint32_t type) {
	// L'ordre compte : un Pendule peut aussi etre Synchro/Xyz/Lien, et c'est le
	// mecanisme d'EXTRA DECK qui impose l'evenement.
	if(type & kTypeFusion)  return Mech::Fusion;
	if(type & kTypeSynchro) return Mech::Synchro;
	if(type & kTypeXyz)     return Mech::Xyz;
	if(type & kTypeLink)    return Mech::Link;
	if(type & kTypeRitual)  return Mech::Ritual;
	if(type & kTypeMonster) return Mech::MainMonster;
	return Mech::SpellTrap;
}

bool FromExtraDeck(Mech m) {
	return m == Mech::Fusion || m == Mech::Synchro || m == Mech::Xyz ||
		   m == Mech::Link;
}

struct TargetCount {
	uint32_t code = 0;
	uint32_t need = 0;      // exemplaires exiges par le board cible
	uint32_t have = 0;      // exemplaires dans la decklist (main + extra)
	Mech mech = Mech::SpellTrap;
	bool token = false;
};

// Rend les comptes par carte cible, et remplit `events` : mecanisme -> nombre
// d'EVENEMENTS d'invocation exiges.
std::vector<TargetCount> CountTarget(const BoardKey& target, const Deck& deck,
									 const CardDB& db,
									 std::map<Mech, uint32_t>& events) {
	std::map<uint32_t, uint32_t> need;
	for(uint32_t c : target.codes)
		++need[db.Canonical(c)];

	std::map<uint32_t, uint32_t> have;
	for(const auto* list : { &deck.main, &deck.extra })
		for(uint32_t c : *list)
			++have[db.Canonical(c)];

	std::vector<TargetCount> out;
	for(const auto& [code, n] : need) {
		TargetCount t;
		t.code = code;
		t.need = n;
		auto it = have.find(code);
		t.have = it == have.end() ? 0u : it->second;
		const CardRow* row = db.Find(code);
		t.mech = row ? MechOf(row->type) : Mech::SpellTrap;
		t.token = row && (row->type & kTypeToken);
		// Un Token n'est pas dans la decklist et ne s'invoque pas : il est
		// PRODUIT par un effet. Le compter comme une invocation manquante
		// serait un faux positif garanti.
		if(!t.token && FromExtraDeck(t.mech))
			events[t.mech] += n;
		out.push_back(t);
	}
	std::sort(out.begin(), out.end(),
			  [](const TargetCount& a, const TargetCount& b) {
				  if(a.need != b.need) return a.need > b.need;
				  return a.code < b.code;
			  });
	return out;
}

// Rapport, et verdict de faisabilite — un DOUTE, jamais un arret.
// Rend le nombre de cartes dont la decklist ne peut pas fournir les copies.
size_t ReportTargetCounting(const std::vector<TargetCount>& counts,
							const std::map<Mech, uint32_t>& events,
							const CardDB& db) {
	std::printf("\n--- comptage derive du board cible ---\n");
	std::printf("  %-9s %-6s %-6s %-24s %s\n", "exiges", "deck", "code",
				"mecanisme", "carte");
	size_t short_of = 0;
	for(const TargetCount& t : counts) {
		const bool manque = !t.token && t.have < t.need;
		if(manque)
			++short_of;
		std::printf("  %-9u %-6u %-6u %-24s %s%s\n", t.need,
					t.token ? 0u : t.have, t.code, MechName(t.mech),
					db.Name(t.code).c_str(),
					t.token ? "   (Token : produit par un effet)"
							: (manque ? "   <-- la decklist n'en a pas assez"
									  : ""));
	}
	if(!events.empty()) {
		std::printf("\n  EVENEMENTS d'invocation exiges par le board seul :");
		for(const auto& [m, n] : events)
			std::printf("  %u %s", n, MechName(m));
		std::printf("\n  (on compte les EVENEMENTS, jamais leurs declencheurs :"
					" « trois Fusions » ne veut\n   pas dire « trois "
					"Polymerisations » — une Fusion peut partir d'ailleurs.)\n");
	}
	if(short_of) {
		std::printf("\n  !! FAISABILITE DOUTEUSE : %zu carte(s) cible(s) "
					"exigent plus d'exemplaires que\n     la decklist n'en "
					"contient. Ce n'est PAS un verdict d'impossibilite — une "
					"carte\n     qui COPIE un nom satisfait le but sans etre "
					"une copie physique, et le but se\n     juge sur le code "
					"EFFECTIF. La recherche continue (regle : on pondere, on "
					"ne\n     prune pas).\n", short_of);
	}
	return short_of;
}

// AMORCE DU GRAPHE DE RECETTES PAR LE TEXTE DE CARTE (chantier 16, regle 3).
//
// « Le texte n'est qu'une AMORCE ; la verite vient de l'observation. » La moitie
// observationnelle seule a une limite exacte : elle n'apprend que des
// invocations REUSSIES, et la carte qu'on cherche est precisement celle qu'aucune
// ligne n'a jamais posee. Sans amorce, `Distance` rend son plancher pour elle et
// `h` reste PLAT la ou il devrait renseigner.
//
// Le texte comble ce trou. Sa premiere ligne, pour un monstre d'extra deck, est
// la ligne de materiaux, et son format est regulier :
//     "Lunalight Leo Dancer" + 3 "Lunalight" monsters
//     2 Level 4 monsters
//
// CE QU'ON EN PREND — et la session 10 a du elargir, mesure a l'appui.
//
// La session 9 ne retenait que les materiaux NOMMES ENTRE GUILLEMETS, en jugeant
// les exigences d'archetype et de niveau « presque toujours faciles a satisfaire,
// donc du bruit sans gradient ». Le relevé des dix cartes de l'extra deck de
// l'etalon A dit le contraire :
//
//   Liger Dancer    "Lunalight Leo Dancer" + 3 "Lunalight" monsters
//   Leo Dancer      "Lunalight Panther Dancer" + 2 "Lunalight" monsters
//   Sabre Dancer    3 "Lunalight" monsters
//   Perfume Dancer  2 "Lunalight" monsters
//   Bagooska        2 Level 4 monsters          <- une carte CIBLE
//   Dugares         2 Level 4 monsters
//   ... (Cross-Sheep, A Bao A Qu, Tiger King, Underworld Goddess)
//
// HUIT cartes sur dix ne nomment AUCUNE carte : sans les exigences cardinales,
// l'amorce ne pose qu'une seule recette et le mecanisme est vivant sans effet
// (piege 42). Et l'argument « sans gradient » est faux dans l'autre sens : une
// exigence CARDINALE est precisement ce qui decroit continument — « 3 monstres
// Lunalight » perd une unite a chaque Lunalight pose, c'est-a-dire AVANT
// qu'aucune carte cible ne touche le terrain. C'est le trou du `h` plat.
//
// Restent ignorees, et volontairement : les exigences de type/attribut/race
// (« Beast-Warrior », « Effect Monsters », « including a Fiend monster ») et les
// contraintes de distinction (« 2 monsters with different names »). Les omettre
// SOUS-ESTIME le cout — direction sure au regard de la regle 2.
//
// La zone est le JOKER : le texte nomme un materiau sans dire d'ou il vient.
// Depuis la session 10, ce joker EXCLUT le deck et l'extra deck (cf. kZoneAny) :
// une carte qui y dort n'est pas un materiau disponible.

// Un archetype se nomme dans le texte (« Lunalight »), mais le core ne connait
// que des SETCODES numeriques, et aucune table nom -> setcode n'est disponible
// hors de `strings.conf`. On le resout donc par le DECK lui-meme : les cartes
// dont le nom contient le fragment doivent toutes porter un setcode commun.
// C'est vrai par construction d'un archetype, et verifiable — le setcode retenu
// et son nombre de fournisseurs sont imprimes.
//
// Rend 0 si l'intersection est vide ou si le fragment ne designe pas au moins
// deux cartes : dans le doute, on n'amorce pas (regle 2).
uint16_t SetcodeOfFragment(const CardDB& db, const std::vector<uint32_t>& pool,
						   const std::string& fragment, size_t* providers) {
	std::vector<uint16_t> common;
	size_t matched = 0;
	for(uint32_t code : pool) {
		const std::string name = db.Name(code);
		if(name.find(fragment) == std::string::npos)
			continue;
		const CardRow* row = db.Find(code);
		if(!row)
			continue;
		std::vector<uint16_t> mine;
		for(uint16_t sc : row->setcodes)
			if(sc)
				mine.push_back(sc);
		if(mine.empty())
			return 0;   // une carte du nom sans setcode : fragment non fiable
		if(!matched++) {
			common = mine;
		} else {
			std::vector<uint16_t> keep;
			for(uint16_t sc : common)
				if(std::find(mine.begin(), mine.end(), sc) != mine.end())
					keep.push_back(sc);
			common.swap(keep);
		}
		if(common.empty())
			return 0;
	}
	if(matched < 2 || common.empty())
		return 0;
	if(providers)
		*providers = matched;
	return common.front();
}

// CE QUE L'AMORCE VAUT, IMPRIME ET VERIFIABLE A LA MAIN.
//
// Le §9.16 publiait un tableau de distances amorcees (Liger 2, Leo 1, Bagooska
// 1) qu'AUCUNE sortie du solveur ne produisait — il venait d'une trace hors
// outil, et le piege 64 dit ce que vaut une trace qui ne rejoue pas le code.
// Cette table-ci sort du graphe lui-meme, par le meme `DistanceAll` que le
// finisseur appelle.
//
// L'etat de reference est le TERRAIN VIDE : rien de pose, rien au cimetiere.
// C'est le point de depart de la ligne, et c'est la seule configuration
// definie sans rejouer un duel. Le `h` plat y vaut 1 par carte cible manquante,
// par construction — la colonne de droite dit donc immediatement si l'amorce
// ajoute quoi que ce soit, et de combien.
void ReportSeededDistances(const BoardKey& target, const RecipeGraph& graph,
						   const CardDB& db,
						   const std::vector<uint32_t>& watched = {}) {
	if(target.codes.empty())
		return;
	// Presence VIDE : aucune entite nulle part. Toute exigence est donc a
	// satisfaire, et la distance affichee est celle du depart.
	struct NoAvail {
		uint32_t Count(const Requirement&) const { return 0u; }
		bool Claim(const Requirement&) { return false; }
		uint32_t CountAndClaim(const Requirement&) { return 0u; }
		void ResetClaims() {}
	} none;
	std::vector<uint32_t> seen;
	std::printf("     %-44s %-8s %s\n", "carte", "h plat",
				"distance amorcee");
	// Les cartes SURVEILLEES sont imprimees avec les cibles, et pour la meme
	// raison : la sonde de repetition mesure des distances a CES cartes-la, et
	// une ligne au plancher previent que la sonde n'aura rien a dire — avant le
	// run, pas apres.
	auto row = [&](uint32_t code, const char* tag) {
		const uint32_t c = db.Canonical(code);
		if(std::find(seen.begin(), seen.end(), c) != seen.end())
			return;
		seen.push_back(c);
		const std::vector<uint32_t> one{ c };
		const uint32_t d = graph.DistanceAll(one, 0x0c /* terrain */, none);
		std::printf("     %-44s %-8u %u%s%s\n", db.Name(c).c_str(), 1u, d,
					d > 1 ? "   <-- gradient" : "   (plancher : rien a dire)",
					tag);
	};
	for(uint32_t code : target.codes)
		row(code, "");
	for(uint32_t code : watched)
		row(code, "   [surveillee]");
}

// SONDE DE REPETITION (session 16) — l'impression, une fois par PHASE.
//
// Elle est imprimee separement pour les tirages et pour le finisseur, et ce
// n'est pas de la cosmetique : 9.21 (d) a coute une lecture fausse parce qu'un
// juge ne couvrait que la phase tirages alors que la conversion se faisait dans
// les tirages ENRACINES. Un « jamais » de la premiere table ne vaut donc que
// pour elle.
// `card_id` / `yn_id` : `Choice::card` est-il renseigne sur les prompts de
// SELECTION et sur les prompts OUI/NON ? Ils valent desormais TOUJOURS vrai —
// l'identite y est inconditionnelle depuis la session 18ter — mais les gardes
// restent, et ce n'est pas de la superstition : SANS eux, les compteurs de choix
// valent structurellement ZERO et s'impriment « JAMAIS RETENUE », ce qui se lit
// comme un fait. Le defaut a failli produire une conclusion fausse en seance ;
// le garde reste pour que le jour ou quelqu'un rend l'identite conditionnelle a
// nouveau, la sonde le DISE au lieu de mentir.
void PrintRepeatProbe(const RepeatProbe rep[4], uint64_t rollouts,
					  const CardDB& db, bool card_id, bool yn_id,
					  const char* phase) {
	std::printf("\n  --- sonde de repetition (--probe-repeat), %s : %llu "
				"tirage(s) ---\n", phase, (unsigned long long)rollouts);
	bool any = false;
	for(int i = 0; i < 4; ++i) {
		const RepeatProbe& r = rep[i];
		if(!r.code)
			continue;
		any = true;
		std::printf("  %s : >=1 %llu  >=2 %llu  >=3 %llu  >=4 %llu  >=5 %llu%s\n",
					db.Name(r.code).c_str(),
					(unsigned long long)r.reached[0],
					(unsigned long long)r.reached[1],
					(unsigned long long)r.reached[2],
					(unsigned long long)r.reached[3],
					(unsigned long long)r.reached[4],
					r.reached[0] == 0
						? "  <-- JAMAIS : aucune invocation dans cette phase"
						: "");
		// SONDE D'OFFRE (session 17) — LA DECOMPOSITION DE LA LOI D'ARITE.
		// Elle est imprimee AVANT tout le reste parce qu'elle decide de quel
		// chantier releve la panne, et qu'une session entiere (la 16) a conclu
		// « le solveur n'y va jamais » sans savoir si le jeu le lui proposait.
		{
			const double per = rollouts ? double(r.offer_rollouts) * 100.0 /
											  double(rollouts)
										: 0.0;
			std::printf("      OFFRE : proposee dans %llu tirage(s) (%.2f %%), "
						"%llu decision(s)\n",
						(unsigned long long)r.offer_rollouts, per,
						(unsigned long long)r.offer_steps);
			static const char* kOfferNames[7] = { "IDLECMD", "SELECT_CARD",
												  "UNSELECT", "SUM", "CHAIN",
												  "POSITION", "OUI/NON" };
			// ACTIVATIONS : le seul chiffre qui dise si le solveur a essaye la
			// PORTE, pour une carte dont le role est d'ouvrir une voie plutot
			// que d'etre posee (Wolf, Masquerade — et Leo Dancer, qui n'est
			// jamais invocable par la voie normale, son materiau nomme etant
			// absent du deck).
			std::printf("      ACTIVEE dans %llu tirage(s) (%.2f %%), %llu fois "
						"au total%s\n",
						(unsigned long long)r.act_rollouts,
						rollouts ? double(r.act_rollouts) * 100.0 / double(rollouts)
								 : 0.0,
						(unsigned long long)r.act_total,
						r.act_rollouts ? "" : "   <-- JAMAIS ACTIVEE");
			// PRESENCE EN ZONE — le seul volet qui parle d'ETATS. Pour une carte
			// dont le role est d'ARRIVER quelque part (Leo Dancer au cimetiere,
			// d'ou il sera banni comme materiau), c'est LA mesure : « jamais
			// invoquee » ne disait pas si elle avait atteint sa zone.
			{
				bool any_zone = false;
				for(int z = 0; z < 6; ++z)
					if(r.zone_rollouts[z]) { any_zone = true; break; }
				std::printf("      ATTEINT :");
				if(!any_zone) {
					std::printf("  aucune zone   <-- la carte n'a JAMAIS BOUGE\n");
				} else {
					for(int z = 0; z < 6; ++z)
						if(r.zone_rollouts[z])
							std::printf("  %s %llu (%.1f %%)", ZoneSlotName(z),
										(unsigned long long)r.zone_rollouts[z],
										rollouts ? double(r.zone_rollouts[z]) *
													   100.0 / double(rollouts)
												 : 0.0);
					std::printf("\n");
				}
			}
			// CONVERSION OFFRE -> CHOIX : le juge exploitable. Il compte des
			// OCCASIONS (des milliers) la ou « la carte a atteint sa zone »
			// compte des EVENEMENTS (des centaines), et c'est ce qui le rend
			// lisible malgre le bruit inter-run.
			if(r.offer_steps && !card_id)
				std::printf("      CHOISIE quand offerte : INDISPONIBLE — ce "
							"compteur exige --card-on-select.\n"
							"                              Sans lui "
							"`Choice::card` est NUL sur les prompts de "
							"SELECTION,\n                              donc le "
							"compte vaut structurellement ZERO et ne dit "
							"RIEN.\n");
			else if(r.offer_steps)
				std::printf("      CHOISIE quand offerte : %llu / %llu   "
							"conversion %.2f %%%s\n",
							(unsigned long long)r.taken_steps,
							(unsigned long long)r.offer_steps,
							100.0 * double(r.taken_steps) / double(r.offer_steps),
							r.taken_steps ? "" : "   <-- JAMAIS RETENUE");
			std::printf("        par prompt :");
			for(int k = 0; k < 7; ++k)
				if(r.offer_by[k])
					std::printf("  %s %llu", kOfferNames[k],
								(unsigned long long)r.offer_by[k]);
			std::printf("\n");
			// LE VOLET OUI/NON (session 18ter). Une defausse FACULTATIVE qui
			// DEBLOQUE une voie ne se lit dans AUCUN autre compteur : le prompt
			// est offert, le solveur repond, et refuser ne coute rien de
			// visible — ni au board, ni au score, ou une defausse vaut +1 de
			// `fodder` contre +100 pour une carte cible posee. Sur l'etalon A
			// c'est pourtant la decision qui ouvre l'acces au CIMETIERE pour
			// toutes les Fusions suivantes. Exige --yn-identity, sans quoi le
			// prompt est anonyme et la sonde ne peut l'attribuer a personne.
			if(!yn_id && r.offer_by[6])
				std::printf("        OUI/NON : INDISPONIBLE — exige "
							"--yn-identity (le prompt est anonyme sans lui)\n");
			if(r.yn_steps)
				std::printf("        OUI/NON : %llu offre(s), OUI retenu %llu "
							"fois (%.1f %%)%s\n",
							(unsigned long long)r.yn_steps,
							(unsigned long long)r.yn_yes,
							100.0 * double(r.yn_yes) / double(r.yn_steps),
							r.yn_yes ? "" : "   <-- JAMAIS OUI");
			// LE VERDICT NE SE LIT PAS SUR LE TOTAL, et c'est la correction la
			// plus importante de la sonde. `SELECT_CARD` est AMBIGU : il porte
			// « choisis ta Fusion parmi celles payables » aussi bien que
			// « regarde ton extra deck ». Les prompts NON ambigus sont
			// `IDLECMD` (invoquer depuis la main ou l'extra) et `POSITION` (la
			// carte est POSEE — preuve directe). S'ils sont a zero, la carte
			// n'a jamais ete invocable, quel que soit le total.
			const uint64_t real = r.offer_by[0] + r.offer_by[5];
			if(!r.offer_rollouts) {
				std::printf("        <-- JAMAIS PROPOSEE, sur aucun prompt.\n");
			} else if(!real && !r.reached[0]) {
				std::printf("        <-- JAMAIS INVOCABLE. Les %llu offre(s) sont "
							"toutes sur des prompts de SELECTION,\n"
							"            aucune sur IDLECMD ni POSITION : le pool "
							"la CONTIENT sans qu'elle soit payable\n"
							"            (un pool qui liste tout l'extra deck). La "
							"panne est dans l'ETAT — chantiers 3\n"
							"            (--recipe-w) et 4 (--backward), pas dans "
							"l'echantillonnage.\n",
							(unsigned long long)r.offer_steps);
			} else if(!r.reached[0]) {
				std::printf("        <-- INVOCABLE ET JAMAIS PRISE (%llu offre(s) "
							"sur IDLECMD/POSITION). La panne est\n"
							"            dans l'ECHANTILLONNAGE : troncature des "
							"sous-ensembles ou poids de politique.\n"
							"            Chantiers 1 (--assign) et 2 "
							"(--hindsight).\n",
							(unsigned long long)real);
			} else {
				const double take = 100.0 * double(r.reached[0]) /
									double(r.offer_rollouts);
				std::printf("        conversion offre -> invocation : %.1f %% des "
							"tirages qui l'ont vue\n", take);
			}
		}
		if(!r.more_n) {
			std::printf("      (aucune premiere invocation : rien a sonder)\n");
			continue;
		}
		char refd0[32], refrest[32];
		if(r.d0 == 0xffffffffu)
			std::snprintf(refd0, sizeof refd0, "non mesuree");
		else
			std::snprintf(refd0, sizeof refd0, "%u", r.d0);
		if(r.rest0 == 0xffffffffu)
			std::snprintf(refrest, sizeof refrest, "non mesuree");
		else
			std::snprintf(refrest, sizeof refrest, "%u", r.rest0);
		std::printf("      distance de recettes a un exemplaire DE PLUS, prise A "
					"LA 1re invocation :\n"
					"        moyenne %.2f, min %u, max %u   (reference depuis "
					"l'etat de depart : %s, %llu releve(s))\n",
					r.more_sum / double(r.more_n), r.more_min, r.more_max, refd0,
					(unsigned long long)r.d0_samples);
		// GARDE-FOU DU PLANCHER (piege 42, et la lecon la plus chere du dossier :
		// un diagnostic qui repond a cote de sa propre question). Le graphe de
		// recettes rend 1 pour tout produit dont il ne connait AUCUNE recette —
		// c'est la regle 2, et c'est voulu. Mais alors « distance 1 <= reference
		// 1 » n'est pas un materiau conserve : c'est un graphe MUET. Rendre un
		// verdict la-dessus serait fabriquer une conclusion a partir d'une
		// absence de mesure.
		if(!r.known) {
			std::printf("        <-- PLANCHER : le graphe ne connait AUCUNE "
						"recette pour cette carte (regle 2).\n"
						"            La distance ne peut RIEN dire ici — aucun "
						"verdict n'est rendu.\n");
		} else if(r.more_max <= 1) {
			// RESERVE MESUREE, et elle interdit le verdict tout autant qu'un
			// plancher. Les recettes AMORCEES PAR LE TEXTE portent la zone
			// JOKER (kZoneAny), qui accepte le cimetiere. Or les materiaux que
			// l'invocation vient de consommer y sont justement arrives : ils
			// comptent donc encore comme disponibles, et « un exemplaire de
			// plus » parait toujours a une invocation pres. Une distance
			// uniformement egale a 1 est la signature de ce biais, pas la
			// preuve d'un materiau conserve.
			std::printf("        <-- distance uniformement 1 : la recette lue "
						"est AMORCEE (zone joker),\n"
						"            et le materiau qui vient d'etre consomme "
						"compte encore depuis le cimetiere.\n"
						"            Biais connu — aucun verdict n'est rendu "
						"sur cet axe.\n");
		} else if(r.more_kept + r.more_lost) {
			const double kept = 100.0 * double(r.more_kept) /
								double(r.more_kept + r.more_lost);
			std::printf("        materiau CONSERVE (<= reference) : %llu (%.1f %%)"
						"   CONSOMME (> reference) : %llu\n",
						(unsigned long long)r.more_kept, kept,
						(unsigned long long)r.more_lost);
			std::printf("        VERDICT : %s\n",
						kept >= 50.0
							? "le 2e exemplaire est JAMAIS TENTE — le materiau "
							  "reste la, le correctif est dans l'ECHANTILLONNAGE"
							: "le 2e exemplaire est TOUJOURS PERDU — la 1re "
							  "invocation consomme la chaine, le correctif est "
							  "dans le `h`");
		}
		// L'AXE CONSOMMATION, qui vaut aussi pour un but SANS repetition (etalon
		// B) : la distance au reste du board au moment ou cette piece tombe,
		// contre la meme distance au depart. Elle DOIT avoir baisse — une piece
		// posee rapproche du board. Si elle ne baisse pas, poser cette piece a
		// coute ailleurs ce qu'elle a rapporte ici, et c'est exactement le mur.
		std::printf("      distance au RESTE de la cible au meme instant : %.2f  "
					"(au depart : %s)\n"
					"      decision moyenne de la 1re invocation : %.1f  |  "
					"decisions restantes apres : %.1f\n",
					r.rest_sum / double(r.more_n), refrest,
					double(r.first_depth_sum) / double(r.more_n),
					double(r.after_sum) / double(r.more_n));
	}
	if(!any)
		std::printf("  (aucune carte surveillee : --probe-repeat exige "
					"--summon-min ou --resolve)\n");
}

size_t SeedRecipesFromText(const CardDB& db, const Deck& deck,
						   const BoardKey& target, RecipeGraph& graph,
						   bool cardinal,
						   const std::vector<uint32_t>& watched = {}) {
	// Candidats : l'extra deck (les seules cartes a ligne de materiaux), les
	// cartes du board cible, qui peuvent ne pas etre dans la decklist, et les
	// cartes SURVEILLEES (--resolve / --summon-min).
	//
	// Les surveillees ont ete ajoutees en session 16 pour une raison mesuree :
	// la sonde de repetition demande la distance a un exemplaire DE PLUS de la
	// carte surveillee, et une carte hors board cible n'avait AUCUNE recette
	// amorcee — la distance retombait au plancher 1 et la sonde rendait un
	// verdict sur un graphe muet. Le mecanisme etait vivant et sans effet,
	// piege 42 a l'identique.
	std::vector<uint32_t> candidates;
	for(uint32_t c : deck.extra)
		candidates.push_back(db.Canonical(c));
	for(uint32_t c : target.codes)
		candidates.push_back(db.Canonical(c));
	for(uint32_t c : watched)
		candidates.push_back(db.Canonical(c));
	std::sort(candidates.begin(), candidates.end());
	candidates.erase(std::unique(candidates.begin(), candidates.end()),
					 candidates.end());

	// DISPONIBLE DANS CE DECK : main + extra. Un materiau nomme qui n'y est pas
	// ne peut pas etre pose par ce deck, et la recette qui l'exige est une ROUTE
	// MORTE — la compter donnerait un cout fonde sur un chemin impossible.
	//
	// Le cas est reel et il a ete trouve en lisant une trace : « Lunalight Leo
	// Dancer » n'a qu'une ligne de materiaux, « "Lunalight Panther Dancer" +
	// 2 "Lunalight" monsters », et Panther Dancer N'EST PAS dans le deck de
	// l'etalon A. Or Leo y est bel et bien invocable — par substitut de Fusion,
	// par copie de nom, par un effet qui ignore les materiaux. Le texte decrit
	// UNE voie, pas LA voie.
	//
	// On retombe donc au plancher pour ce produit : « on ne sait pas comment il
	// arrive » est plus vrai que « il coute le prix d'une route impossible ».
	// C'est la regle 2 appliquee au sens strict — on n'invente pas de cout, et
	// on ne declare rien inatteignable non plus.
	std::vector<uint32_t> available;
	for(const auto* list : { &deck.main, &deck.extra })
		for(uint32_t c : *list)
			available.push_back(db.Canonical(c));
	std::sort(available.begin(), available.end());
	available.erase(std::unique(available.begin(), available.end()),
					available.end());
	auto in_deck = [&available](uint32_t c) {
		return std::binary_search(available.begin(), available.end(), c);
	};

	size_t seeded = 0, dead_routes = 0, named = 0, arch = 0, lvl = 0;
	std::vector<std::string> announced;   // fragments d'archetype deja imprimes
	for(uint32_t code : candidates) {
		// SEULES les cartes d'EXTRA DECK ont une ligne de materiaux. Pour toute
		// autre, la premiere ligne du texte est de la PROSE — et une prose
		// contient volontiers « Special Summon 1 Level 4 monster », que
		// l'analyse ci-dessous prendrait pour une exigence. On amorcerait alors
		// une recette a partir d'une phrase, ce qui n'est plus une amorce mais
		// une invention (regle 2).
		const CardRow* prow = db.Find(code);
		if(!prow || !(prow->type & (kTypeFusion | kTypeSynchro | kTypeXyz |
									kTypeLink)))
			continue;
		const std::string& line = db.MaterialLine(code);
		if(line.empty())
			continue;
		std::vector<Requirement> mats;
		bool dead = false;
		// Analyse par jetons. Un nombre en tete qualifie ce qui SUIT :
		//     3 "Lunalight" monsters   ->  archetype Lunalight, count 3
		//     2 Level 4 monsters       ->  niveau 4, count 2
		//     "Lunalight Leo Dancer"   ->  carte nommee, count 1
		// Un nombre suivi d'autre chose (« 2+ monsters », « 4+ Effect Monsters »)
		// est JETE : on n'en tire aucune exigence, ce qui sous-estime.
		uint32_t pending = 0;   // 0 = aucun nombre en attente
		for(size_t i = 0; i < line.size() && !dead;) {
			if(std::isdigit(static_cast<unsigned char>(line[i]))) {
				uint32_t n = 0;
				while(i < line.size() &&
					  std::isdigit(static_cast<unsigned char>(line[i])))
					n = n * 10 + static_cast<uint32_t>(line[i++] - '0');
				pending = n;
				continue;
			}
			if(line[i] == '"') {
				const size_t b = line.find('"', i + 1);
				if(b == std::string::npos)
					break;
				const std::string name = line.substr(i + 1, b - i - 1);
				const uint32_t want = pending ? pending : 1u;
				i = b + 1;
				pending = 0;
				const uint32_t mc = db.CodeByExactName(name);
				if(mc && mc != code) {
					if(!in_deck(mc)) {
						dead = true;   // route morte (piege 63)
						break;
					}
					// « 2 "Nom" » devient DEUX exigences d'une copie, pas une
					// exigence de deux : une carte nommee se resout par une
					// recherche de presence (0 ou 1) et par recursion sur SA
					// recette, deux choses qu'un compteur ne sait pas faire.
					// Le champ `count` reste ainsi toujours 1 pour kReqCard —
					// c'est ce qui autorise le memo de Distance a l'ignorer.
					for(uint32_t k = 0; k < want; ++k)
						mats.push_back(Requirement{ mc, kZoneAny, kReqCard, 1 });
					++named;
					continue;
				}
				if(mc)          // le produit se nomme lui-meme : rien a exiger
					continue;
				// Aucune carte de ce nom : c'est un fragment d'ARCHETYPE.
				if(!cardinal)
					continue;
				size_t providers = 0;
				const uint16_t sc =
					SetcodeOfFragment(db, available, name, &providers);
				if(!sc)
					continue;   // fragment non resolu : on n'invente rien
				// Le setcode est DEDUIT du deck, pas lu dans une table : il est
				// imprime avec son nombre de fournisseurs pour etre verifiable.
				if(std::find(announced.begin(), announced.end(), name) ==
				   announced.end()) {
					announced.push_back(name);
					std::printf("     archetype « %s » -> setcode 0x%x, %zu "
								"fournisseur(s) au deck\n", name.c_str(), sc,
								providers);
				}
				mats.push_back(Requirement{ sc, kZoneAny, kReqSetcode,
											static_cast<uint8_t>(want) });
				++arch;
				continue;
			}
			// « Level N » : le niveau exige suit le mot.
			if(cardinal && pending &&
			   line.compare(i, 6, "Level ") == 0) {
				size_t j = i + 6;
				uint32_t m = 0;
				while(j < line.size() &&
					  std::isdigit(static_cast<unsigned char>(line[j])))
					m = m * 10 + static_cast<uint32_t>(line[j++] - '0');
				if(m) {
					mats.push_back(Requirement{ m, kZoneAny, kReqLevel,
												static_cast<uint8_t>(pending) });
					++lvl;
					pending = 0;
					i = j;
					continue;
				}
			}
			// Tout mot ordinaire consomme le nombre en attente : « 2+ monsters,
			// including a Fiend monster » ne doit pas voir son « 2 » recolle a
			// un fragment plus loin dans la ligne.
			if(std::isalpha(static_cast<unsigned char>(line[i]))) {
				while(i < line.size() &&
					  (std::isalpha(static_cast<unsigned char>(line[i])) ||
					   line[i] == '-'))
					++i;
				pending = 0;
				continue;
			}
			++i;
		}
		if(dead) {
			++dead_routes;
			continue;
		}
		if(mats.empty())
			continue;
		// `primed` : cette recette vient du TEXTE. Elle sera ecartee des qu'une
		// invocation reelle du meme produit aura ete observee (regle 3).
		graph.Observe(code, mats, /*primed=*/true);
		++seeded;
	}
	if(dead_routes)
		std::printf("     (%zu recette(s) ecartee(s) : elles nomment un materiau "
					"absent de ce deck)\n", dead_routes);
	if(seeded)
		std::printf("     exigences posees : %zu nommee(s), %zu archetype(s), "
					"%zu niveau(x)%s\n", named, arch, lvl,
					cardinal ? "" : "   (--no-seed-quant : cardinales ETEINTES)");
	return seeded;
}

void PrintCuts(const CutCounts& c) {
	std::printf("           elagage : contrainte %llu, garde %llu, tour %llu, "
				"borne %llu, partition %llu, sous-ens. %llu\n",
				(unsigned long long)c.constraint, (unsigned long long)c.guard,
				(unsigned long long)c.turn, (unsigned long long)c.bound,
				(unsigned long long)c.claim, (unsigned long long)c.subsets);
	// Vie de --no-self-negate (s22ter) : options de chaine retirees.
	if(c.selfneg)
		std::printf("           discipline : %llu negation(s) sur soi "
					"retiree(s) (--no-self-negate)\n",
					(unsigned long long)c.selfneg);
	// `impasses` est le symptome n°1 du jeu de scripts decale — la defaillance
	// que ce depot redoute le plus — et il n'etait imprime qu'en mode --width,
	// c'est-a-dire muet exactement la ou elle se produirait (1.9). `nouveaute`
	// dit si le terme de departage des tirages compte encore pour quelque chose
	// ou s'il est sature (1.4) ; `atomes` est la largeur mesuree (1.5).
	std::printf("           sante   : impasses %llu, terminaux %llu, "
				"nouveaute %llu/%llu, atomes %zu\n",
				(unsigned long long)c.dead_ends,
				(unsigned long long)c.terminals, (unsigned long long)c.novel,
				(unsigned long long)(c.novel + c.stale), c.atoms);
	if(c.forced) {
		std::printf("           !! %llu prompt(s) reduits a LA reponse par "
					"defaut (types :", (unsigned long long)c.forced);
		for(int b = 0; b < 64; ++b)
			if(c.forced_mask & (1ull << b))
				std::printf(" %d", b);
		std::printf(")\n              La branche SURVIT, reduite a un seul "
					"choix : tout le reste de ce prompt est hors d'atteinte.\n");
	}
	// SEPARE DU PRECEDENT (audit 18) : ici il n'existe aucune reponse par
	// defaut, donc la branche MEURT — et elle compte AUSSI dans `impasses`
	// ci-dessus. Les confondre faisait lire un seul fait comme deux.
	if(c.killed) {
		std::printf("           !! %llu prompt(s) sans AUCUNE reponse par "
					"defaut : BRANCHE TUEE (types :",
					(unsigned long long)c.killed);
		for(int b = 0; b < 64; ++b)
			if(c.killed_mask & (1ull << b))
				std::printf(" %d", b);
		std::printf(")\n              Ces branches n'ont jamais existe, et "
					"elles sont DEJA comptees dans « impasses ».\n");
	}
	if(c.num_broken)
		std::printf("           !! %llu debordement(s) arithmetiques sqrt-LTS : "
					"ce bras est a JETER\n", (unsigned long long)c.num_broken);
	// GRAPHE DE RECETTES : sans ces deux chiffres le mecanisme serait invisible
	// (piege 52). `invocations observees` a zero = le graphe est VIDE, donc la
	// distance vaut exactement le `h` plat et le mecanisme est INERTE — a savoir
	// avant toute conclusion. `h moyen` compare a |cible manquante| dit si le
	// paysage s'est reellement creuse.
	//
	// `observee(s)` compte les OCCURRENCES, rejeux de prefixe compris : une meme
	// invocation revue a chaque re-descente compte a chaque fois. C'est le bon
	// chiffre pour dire « le graphe a-t-il vu quelque chose », pas pour dire
	// « combien de recettes DISTINCTES il connait ».
	if(c.recipes_seen || c.recipe_h_count)
		std::printf("           recettes : %llu invocation(s) observee(s), "
					"h moyen %.2f sur %llu evaluation(s)\n",
					(unsigned long long)c.recipes_seen,
					c.recipe_h_count ? c.recipe_h_sum / double(c.recipe_h_count)
									 : 0.0,
					(unsigned long long)c.recipe_h_count);
	// LA VIE DU MECANISME (piege 52). Un `h` de landmarks allume mais jamais
	// evalue est indiscernable d'un `h` evalue qui ne dit rien : le compte
	// separe les deux, et la moyenne dit si le paysage se creuse — collee au
	// nombre total de landmarks, la recherche n'accomplit RIEN ; collee a zero,
	// les landmarks sont trop faciles et ne guident pas.
	if(c.landmark_h_count)
		std::printf("           landmarks : h moyen %.2f sur %llu evaluation(s)\n",
					c.landmark_h_sum / double(c.landmark_h_count),
					(unsigned long long)c.landmark_h_count);
}

struct Options {
	std::string replay;
	// Replay fournissant la position de DEPART (deck, main, graine). Vide : on
	// cherche dans le duel de la reference elle-meme.
	std::string start_replay;
	std::string workdir = "D:\\ProjectIgnis";
	std::vector<std::string> scriptdirs;
	// Repertoire des replays produits : le livrable demande.
	std::string outdir = "solutions";
	bool verbose = false;
	// Profil du chemin chaud : sondes rdtsc thread_local, imprimees par phase
	// avec la ligne « reste » (chantier perf, etape 1). Le cout de l'instrument
	// se chiffre en comparant deux runs a graine egale, avec et sans.
	bool profile = false;
	int target_player = 0;
	bool no_arena = false;
	bool stop_gc = true;
	size_t arena_mb = 256;
	bool growth = false;          // mesurer la courbe de croissance du graphe
	uint32_t growth_max = 14;
	double growth_ms = 20000;
	bool solve = false;           // recherche guidee vers le board cible
	double solve_ms = 120000;
	unsigned threads = 0;         // 0 = tous les coeurs
	// Elagage par nouveaute : -1 = patience auto-calibree sur la mesure de
	// largeur, 0 = desactive, >0 = patience imposee.
	int novelty = -1;
	bool nrpa = true;             // tirages par politique apprise (NRPA)
	bool width = false;           // mesure de largeur seule
	// Graine des tirages (0 = derivee du temps et imprimee : deux runs a la
	// meme graine explorent en grande partie les memes trajectoires, la
	// constante d'antan faisait de chaque relance le meme run).
	uint64_t seed = 0;
	// Biais GNRPA des coups au repertoire (-1 = defaut du moteur, 1,5).
	double nrpa_bias = -1.0;
	// Persistance partielle de la politique NRPA entre redemarrages
	// (attenuation des poids ; 0 = politique vierge, comportement d'avant).
	double nrpa_keep = 0.5;
	// GNRPA a repetitions limitees (arXiv:2401.10420) : nombre de fois ou la
	// meilleure sequence peut etre re-trouvee avant d'arreter le niveau.
	// 0 = stagnation seule — le DEFAUT, sur mesure : a R=2, la transplantation
	// test 4 (90 s, graine 2611923443488327891) tombe de 8/8 + 36 lignes a
	// 7/8 + 0 ligne ; l'arret precoce des niveaux casse la convergence que la
	// stagnation a 8 laissait aboutir. Le drapeau reste pour re-mesurer.
	// PORTEE REELLE : la phase de tirages seulement. Ni le finisseur enracine,
	// ni les fenetres --fire. Une re-mesure ne porterait donc que sur un tiers
	// du flux (audit §5).
	// ADAPTATION LENTE ET LONGUE (session 14, chantier 2 — recette Montparnasse,
	// arXiv:2505.02110 / 2606.07562, Eterna100 resolu ainsi) : le pas
	// d'adaptation NRPA et le nombre d'iterations par niveau. Jusqu'ici gardes en
	// dur (1.0 et 24) — donc jamais places sur un cadran, donc jamais mesures.
	// La recette du papier est un ALPHA PETIT compense par BEAUCOUP d'iterations
	// au niveau bas : la politique se deplace lentement et explore longtemps le
	// meme bassin au lieu de s'y verrouiller en quelques adaptations. 0 = defaut
	// du moteur (comportement d'avant a l'octet pres).
	double nrpa_alpha = 0;
	uint32_t nrpa_iters = 0;
	// Table de transposition PARTAGEE entre workers (lazy SMP), en Mo par
	// passe. 0 = tables privees (comportement d'avant).
	// Table de transposition PARTAGEE entre workers. PORTEE REELLE : les seules
	// passes LDS (reparation, transplantation). Ni RunLevin — qui garde sa
	// table privee — ni RunNrpa, qui n'en a pas. Le drapeau n'a donc aucun
	// effet sur les deux phases qui consomment le budget (audit §5).
	size_t tt_mb = 64;
	// Finisseur de la transplantation : "levin" (archive Go-Explore + recul +
	// Levin Tree Search sur la politique NRPA), "mono" (l'ancien : fouille
	// guidee du seul meilleur etat — mesure trois fois epuise en ~6 etats),
	// "ab" (les deux a budget egal : la mesure).
	std::string finisher = "levin";
	// Taille de l'archive Go-Explore (etats distincts conserves avec chemin,
	// par worker et comme nombre de racines du finisseur). 0 = pas d'archive.
	size_t archive_k = 16;
	// Budget minimal RESERVE au finisseur (ms). 0 = repartition d'origine
	// (70 % tirages, finisseur 0,8 x le reste plafonne a 240 s). A regler
	// quand la conversion est la question et que des --approach fournissent
	// deja les racines : les tirages n'ont plus a porter tout le budget.
	double finisher_min = 0;
	// Poids PHS* de la distance au but dans le cout du finisseur (0 = Levin
	// pur, aveugle au but — mesure : il re-monte les reculs profonds sans
	// preferer les branches qui ripent).
	double levin_h = 1.0;
	// Rejeux du finisseur (session 12) : pile de plongee complete (GAGNANT —
	// +92 % d'expansions a temps egal sur l'etalon 0, par defaut) et departage
	// LIFO des ex aequo (refute seul, eteint). Voir SearchConfig.
	bool dive_full = true;
	bool lifo_ties = false;
	// Poids d'une resolution exigee dans le gradient des tirages (defaut 250 ;
	// 100 = l'ancien poids, une carte cible — mesure perdant : les lignes 8/8
	// sans rip gagnaient la course d'adaptation contre les rip-partielles).
	double resolve_weight = 250.0;
	// OPTIMISATION DE COUT anytime (--optimize) : la recherche ne s'arrete
	// plus a la premiere solution — chaque solution resserre la borne, le
	// score de but NRPA devient lexicographique (brulees, puis actions, puis
	// decisions), les tirages continuent APRES le but (les recuperations
	// reduisent les brulees), l'archive prefere les etats au cout partiel
	// bas, et le finisseur tourne meme quand les tirages ont deja des lignes.
	bool optimize = false;
	// TEST ADVERSE (--fire "carte") : la carte est AJOUTEE a la main adverse
	// et l'adversaire la JOUE — a chaque fenetre ou elle est legale, un essai
	// distinct — puis la recherche enracinee doit refermer le board depuis
	// l'etat post-injection. La garde etait un proxy statique (« un contre
	// est disponible ») ; ce mode est la preuve dynamique (« le contre marche
	// ET le combo se referme »).
	std::string fire_spec;
	// Cartes SACRIFIABLES pour contrer (--fire-spare, repetable) : le board
	// cible SANS ces cartes est aussi accepte au but (arbitrage du joueur :
	// contrer Nibiru par Zalen consomme Junk Signal — et le contreur peut se
	// consommer lui-meme).
	std::vector<std::string> fire_spare_specs;
	double fire_ms = 45000;   // budget de recherche par fenetre d'injection
	// --fire-bake : la carte tiree est CUITE dans l'en-tete des replays
	// produits (inseree dans le deck adverse la ou le pseudo-melange sert la
	// main) — ils se rejouent DEPUIS LEUR FICHIER, donc EDOPro les VISIONNE.
	// En echange, start_hand etant partage, la carte prend la place de la
	// derniere carte de la main adverse d'origine (deplacee vers le deck) :
	// le duel differe du mode par defaut d'une carte de main adverse — la
	// preuve d'alignement tranche s'il reste rejouable.
	bool fire_bake = false;
	// --fire-no-chain : no-chain propre a la CONTINUATION post-injection (les
	// --no-chain globaux y sont leves — piege 37 : chainer sur la menace est
	// le role des gardes). Sert a METTRE EN SCENE un contreur precis :
	// interdire Crystal Wing force la voie Zalen+Junk Signal (mesure : sans
	// cela, le solveur satisfait « Zalen se resout » en l'activant AILLEURS
	// pendant que CW nege Nibiru).
	std::vector<std::string> fire_no_chain_specs;
	// --fire-open : n'injecter qu'aux fenetres OUVERTES (chaine vide) — la
	// carte tiree DEMARRE une chaine (link 1) au lieu d'etre chainee sur nos
	// effets. C'est la vraie menace (verdict du joueur : un Nibiru chaine sur
	// Junk Speeder se nege facilement et ne modele pas l'adversaire reel).
	bool fire_open = false;
	// Marge de la borne brulees (B&B) : les brulees ne sont pas monotones
	// (recuperations reelles, piege 27) — la marge se mesure sur la reference
	// (« brulees max en cours de ligne »). >= 255 = borne inactive.
	uint32_t burn_slack = 6;
	// Graine de la borne : meilleures brulees connues d'avance (0 = aucune).
	uint32_t burn_limit = 0;
	// Partage de la borne brulees ENTRE workers (session 6) : un worker qui
	// ameliore les brulees resserre la coupure B&B chez tous, via un atomique
	// (CAS min a la publication, charge relaxed a la coupure).
	// --no-burn-share desactive, pour l'A/B.
	bool burn_share = true;
	// PRIOR PAR REJEU DE SOLUTIONS (session 6, arXiv:2401.10431) : lignes de
	// corpus (--prior, fichier ou dossier, repetable) dont les plan_key sont
	// releves CHACUNE SUR SON DUEL (piege 21 : jamais rejouees sur le duel de
	// depart) et servis en poids INITIAUX de politique NRPA — une politique
	// qui sait deja ripper, la ou l'echantillonnage vierge fait 1/800k.
	std::vector<std::string> prior_files;
	// Poids d'un coup present dans TOUT le corpus (proportionnel sinon).
	double prior_weight = 2.0;
	// REJEU D'ADAPTATION DU CORPUS (session 7, chantier 5bis — la voie restante
	// de 2401.10431 apres la refutation du prior par POIDS) : les memes lignes,
	// relevees non plus en coups isoles mais en SEQUENCES DE DECISIONS (choix
	// legaux + choisi), adaptees dans la politique par le gradient NRPA avant le
	// premier tirage. --adapt-passes 0 desactive le mecanisme (A/B).
	std::vector<std::string> adapt_files;
	uint32_t adapt_passes = 4;
	// OPTIONS (chantier 17) : taille du catalogue de macros minees dans le
	// corpus --adapt et proposees a l'echantillonnage NRPA. 0 = eteint
	// (comportement d'avant a l'octet pres). La prevision (9.19 (b)) ne
	// justifie que le GROS catalogue : 256/support 2 gagne 8,5 ordres, 16 et
	// 64 sont contre-productifs.
	uint32_t options_n = 0;
	uint32_t options_support = 2;
	uint32_t options_len = 8;
	// Fenetre de proposition (v3) : une macro n'est proposee qu'a +/- window
	// decisions enregistrees de sa position d'origine dans le corpus.
	// 0 = pas de garde. REFUTEE (9.19 (g)) : gardee pour l'A/B.
	uint32_t options_window = 0;
	// Garde SEMANTIQUE (la forme designee par 9.19 (g)) : une macro n'est
	// proposee que si le contexte courant (cartes cibles posees, main) est
	// compatible avec une occurrence du corpus — cartes posees exactes, main
	// a +/- options_ctx. -1 = garde eteinte (defaut).
	int options_ctx = -1;
	// MINAGE EN LIGNE (session 14, chantier 1 — Marvin arXiv:1110.2736) :
	// periode en SECONDES du re-minage sur les meilleures lignes DU RUN.
	// 0 = eteint (le catalogue est mine une fois au demarrage sur --adapt, et
	// ne bouge plus : comportement d'avant a l'octet pres). C'est le mecanisme
	// qui rend le bootstrap possible EN UNE SEULE TRAITE — sans corpus externe,
	// sans --approach herite, sans relance.
	uint32_t options_online = 0;
	// Corpus vivant : lignes retenues au total, et par worker (la POMPE A
	// DIVERSITE — en multi-runs elle venait des graines).
	uint32_t options_pool = 12;
	uint32_t options_per_worker = 2;
	// PHS* canonique (audit s12) : cout (d + h)/pi du papier au lieu de notre
	// log(d+1) + h - log pi (facteur e^h, sans garantie).
	// Depilage fusionne de l'arene au retour vers l'ancetre (voir
	// SearchConfig::merged_pop). GAGNANT etalon 0, par defaut.
	bool merged_pop = true;
	// Recuperation d'apres-but dans le finisseur (9.18 (g), opt-in) : sous
	// --optimize, un noeud-but de RunLevin continue au lieu de s'arreter.
	bool finisher_post_goal = false;
	// ARETES MACRO dans le finisseur (session 14, chantier 3) : les macros du
	// catalogue deviennent des aretes de l'arbre de Levin. Opt-in : le controle
	// de l'etalon 0 change LEGITIMEMENT de forme quand il est allume (voir
	// SearchConfig::finisher_options).
	bool finisher_options = false;
	// POLITIQUE A DEUX NIVEAUX (session 7, chantier 5ter — MCPS 2510.06381) :
	// retenue du niveau contextuel, s = n/(n+k). Negatif = eteint.
	double ctx_shrink = -1.0;
	// CONDITIONNEMENT PAR LE CHEMIN (session 14) : le contexte du niveau
	// contextuel devient la somme des coups joues sur les k premieres decisions,
	// au lieu du descripteur (cartes posees, main). 0 = eteint.
	//
	// C'est le retour au critere de MCPS, que ce projet CITAIT sans l'appliquer :
	// son conditionnement est « les parties qui contiennent tous les coups du
	// chemin », le notre etait un descripteur a deux axes qui vaut (0, 3) pour
	// TOUTES les branches a la premiere decision — donc incapable, par
	// construction, de distinguer deux ouvertures l'une de l'autre.
	// BANDIT DE TETE A STATISTIQUE DE PERMUTATION (session 15, --qhat) : la
	// regle de selection de MCPS — argmax de (n Q + n^ Q^)/(n + n^), poids
	// proportionnels aux effectifs — sur les k premieres decisions du tirage.
	// C'est le mecanisme du papier POUR DE VRAI (moyennes de recompense sur des
	// ensembles de tirages, y compris les MORTS), la ou --mcps n'en avait pris
	// que le conditionnement et l'avait pose sur des logits NRPA. 0 = eteint.
	uint32_t qhat_depth = 0;
	uint32_t qhat_window = 4096;
	uint32_t qhat_rho = 32;
	size_t qhat_nodes = 65536;
	// SONDE du bandit : imprimer la table de la RACINE (premiere decision) en
	// fin de phase tirages. Allumee d'office sous --qhat — la courbe d'accord
	// du corpus ne PEUT PAS juger Q^ (elle mesure la reproduction d'un corpus
	// qui ne contient que des bonnes lignes), donc c'est le seul instrument
	// gratuit qui dise si le mecanisme separe quoi que ce soit.
	bool qhat_probe = true;
	// ELAGAGE PAR NOUVEAUTE DANS LES TIRAGES SOUS POLITIQUE (repare s15) : le
	// verdict de nouveaute est deja calcule a chaque decision de PolicyRollout
	// et jete apres un simple departage de score. Opt-in — le mecanisme a un
	// mode de defaillance documente (9.3) et il doit se juger, pas se supposer.
	// Sorties de phase (Battle/End) retirees de l'enumeration : le board cible
	// est celui de la FIN DU TOUR 1, donc changer de phase ne peut que
	// raccourcir la ligne. Le drapeau existait dans EnumOptions sans aucun
	// cadran, et il n'etait lu qu'au prompt idle — au prompt de bataille les
	// deux sorties etaient emises inconditionnellement (repare s15).
	// Une seule zone libre representative par type de zone : declare et
	// documente depuis des sessions, ALLUME NULLE PART (9.20 (e)).
	bool canonical_zones = false;
	// QUOTA PAR NIVEAU DE PROGRES dans l'archive Go-Explore (s15) : rend a
	// l'archive sa nature de COUVERTURE quand sa cle de tri sature.
	// BUT PAR INCLUSION (s15) : le board final doit CONTENIR la cible au lieu
	// de lui etre EGAL. Voir SearchConfig::goal_subset.
	//
	// PAR DEFAUT depuis la s15 des que la cible est POSEE (--target) : une
	// cible posee veut dire « je veux ces cartes », pas « ces cartes et le
	// terrain vide autour ». L'ancien defaut exigeait une zone S/T VIDE et
	// rendait l'etalon A insatisfiable (main de trois magies CONTINUES).
	// Une cible CAPTUREE garde l'egalite exacte : elle porte ses propres S/T.
	bool target_subset = false;   // --target-subset : forcer l'inclusion
	bool target_exact = false;    // --target-exact  : forcer l'egalite
	// Plafond d'entrees du niveau contextuel, par worker (0 = illimite).
	size_t ctx_max = 262144;
	// Temperature de l'echantillonnage NRPA (1.0 = comportement d'avant).
	double nrpa_temp = 1.0;
	// Niveau d'imbrication NRPA. 0 = defaut historique, choisi par un seuil de
	// 180 s sur le budget des tirages — un seuil qui change l'ALGORITHME
	// (iters^2 contre iters^3) sans qu'aucune mesure ne l'adosse, et qui separe
	// exactement les deux commandes comparees dans plusieurs A/B des sessions
	// 5-7 (C15).
	int nrpa_level = 0;
	// Poids du canal par lequel la CONNAISSANCE DU JOUEUR entre dans
	// l'echantillonnage : les cartes --resolve/--summon-min le recoivent
	// d'office. Son voisin nrpa_bias_known a --nrpa-bias depuis la session 3 ;
	// celui-ci n'avait rien, et le §9.14 chiffre la contribution du biais
	// `known` sans jamais isoler celui-ci (2.7). Negatif = defaut du moteur.
	double hint_bias = -1.0;
	// MODE DETERMINISTE (audit 18) : budget en TIRAGES par worker, au lieu du
	// temps de mur. Combine a `--threads 1`, deux executions font exactement le
	// meme travail — c'est le seul mode ou un A/B fin veut dire quelque chose.
	// 0 = illimite (comportement d'avant a l'octet pres).
	uint64_t max_rollouts = 0;
	uint64_t max_nodes = 0;
	// TRONCATURE DU GRADIENT AU PIC DU SCORE. Cf. NrpaRun::peak_steps.
	//
	// DEFAUT DEPUIS LA SESSION 19. Mesure sur les DEUX etalons : x2,6 sur
	// l'arite 3 dans deux paires independantes de l'etalon A (9.26 (f)), et sur
	// l'etalon B en PROPORTION sur dix runs par bras, la pile porte `>=2` de
	// 3/10 a 9/10 (9.28 (e)). Le drapeau devient NEGATIF (`--no-adapt-to-peak`)
	// pour que l'A/B reste possible — regle 2 du README.
	bool adapt_to_peak = true;
	// Nombre maximal de sous-ensembles emis par prompt de selection. C'est ce
	// qui plafonne le facteur de branchement de TOUS les prompts de selection ;
	// il etait ecrit en dur (24) a douze endroits, sans drapeau ni mesure, et
	// le defaut de la structure (64) n'etait jamais utilise (C16). Son effet se
	// lit dans la colonne « sous-ens. » de la ligne d'elagage.
	uint32_t max_subsets = 24;
	// Deriver --summon-min du BOARD CIBLE au lieu de l'ecrire a la main
	// (chantier 16, premier pas). Opt-in : une contrainte derivee change le
	// comportement de la recherche, elle ne doit pas s'imposer en silence.
	bool derive_summon_min = false;
	// GRAPHE DE RECETTES (chantier 16). Trois etats :
	//   negatif : eteint, comportement d'avant a l'octet pres ;
	//   0.0     : le graphe est ALIMENTE et MESURE, mais n'entre pas dans le
	//             cout — c'est le mode qui chiffre ce qu'il saurait dire AVANT
	//             de le laisser decider (piege 40) ;
	//   > 0     : la distance de recettes entre dans `h` avec ce poids.
	double recipes = -1.0;
	// Amorcer le graphe avec les materiaux NOMMES par le texte de carte
	// (regle 3 : le texte est une AMORCE, l'observation est la verite).
	// Sans amorce le graphe n'apprend que des invocations REUSSIES — et
	// la carte cherchee est justement celle qu'on ne reussit jamais.
	bool seed_recipes = true;
	// Amorcer AUSSI les exigences CARDINALES (« 3 "Lunalight" monsters »,
	// « 2 Level 4 monsters »). Separe de seed_recipes pour que l'A/B puisse
	// isoler ce qu'elles apportent : sans elles l'amorce ne pose qu'une recette
	// sur l'etalon A (huit cartes de l'extra sur dix ne nomment aucune carte).
	bool seed_cardinal = true;
	// GRAPHE DE LANDMARKS APPRIS (chantier 18, session 16). Corpus de plans
	// RESOLUS d'ou les landmarks sont extraits — fichiers ou dossiers, comme
	// --adapt et --prior.
	//
	// EXPLICITE ET JAMAIS IMPLICITE, et c'est une exigence de mesure : la ligne
	// de reference est un plan resolu legitime (le chantier le dit), mais si le
	// solveur l'aspirait tout seul, un bras « avec landmarks » melangerait deux
	// facteurs — le mecanisme et le retour du repertoire que --no-plan venait
	// d'ecarter. L'operateur designe le corpus, ou il n'y en a pas.
	std::vector<std::string> landmark_files;
	// Poids d'un accomplissement de landmark dans le SCORE DES TIRAGES, en
	// unites de materiel (une carte cible posee vaut 100). 0 = les landmarks
	// sont appris, imprimes et MESURES sans peser (piege 40).
	double landmark_weight = 0.0;
	// Poids du `h` de landmarks dans le finisseur (meme entree que --recipes).
	double landmark_h = 0.0;
	// SONDE DE REPETITION (session 16). Implique `recipes >= 0` : la sonde
	// mesure des DISTANCES sur le graphe de recettes, donc sans graphe elle
	// n'aurait qu'un histogramme et un silence sur la seule question posee.
	// L'implication est appliquee ET imprimee — un drapeau qui en allume un
	// autre sans le dire est la famille de piege que ce dossier catalogue.
	bool probe_repeat = false;
	// HARNAIS D'OPERATEURS DECLARES (session 19, chantier 0). INSTRUMENT, pas
	// mecanisme : il extrait des scripts Lua du deck la table des operateurs
	// (preconditions, produit, etat accorde, consommation), l'imprime, puis
	// CONFRONTE cette table au plan rejoue — chaque activation correspond-elle a
	// un operateur declare, et la sequence est-elle valide sous les
	// preconditions extraites ?
	//
	// POURQUOI IL PASSE AVANT TOUT LE RESTE. Trois sessions ont bati sur un
	// graphe dont personne n'avait verifie qu'il decrivait le jeu : recettes
	// (s16), landmarks (s16), `--backward` (s17), tous nourris par l'OBSERVATION
	// — ce que le solveur a deja reussi — au lieu de la DECLARATION. Le harnais
	// est falsifiable et coute un run ; s'il echoue, le planificateur est sans
	// objet, et c'est ce qu'on veut savoir en premier.
	bool operators = false;
	// CHANTIER 1 (session 19) : ENSEMENCER LE GRAPHE DE RECETTES DEPUIS LES
	// OPERATEURS DECLARES, au lieu du seul TEXTE de carte.
	//
	// Deux apports, et le second est le type de nœud qui manquait :
	//   - les recettes viennent de `Fusion.AddProcMix*` — des CODES, pas une
	//     phrase anglaise a re-resoudre ;
	//   - une arete « ce CODE peut etre ACQUIS » pour chaque `EFFECT_ADD_CODE` /
	//     `EFFECT_CHANGE_CODE` du deck. Le graphe rangeait Leo comme un produit
	//     a FABRIQUER — d'ou les « 2 sous-produits, 0,02 fabrique » de
	//     `--backward` (9.24 (e)) : il essayait de construire une carte non
	//     constructible.
	//
	// DRAPEAU LE TEMPS DE LE MESURER (regle 2 du README), pas plus : passe sur
	// les deux etalons, il devient le defaut et le drapeau devient negatif.
	bool op_recipes = false;
	// SERIALISATION PAR LE BILAN MATIERE (9.31). Un mecanisme est un drapeau LE
	// TEMPS DE LE MESURER, puis devient le defaut : celui-ci naît donc allume et
	// s'eteint par `--no-serial`, comme `--adapt-to-peak` apres sa promotion.
	// C'est la seule voie que l'arithmetique des 105 ordres laisse ouverte.
	bool serial = true;
	// RETOUR AU BARREAU (s21) : probabilite qu'un tirage NRPA reparte d'une
	// cellule d'archive au lieu de la racine. La moitie de SIW_R qui manquait
	// aux tirages — la forme close de 9.33 (e) dit que `Sigma b^(l_i)` n'existe
	// que si chaque bloc est fouille depuis le barreau precedent. Ne mord que
	// sous serialisation armee. 0 = temoin de l'A/B.
	double reenter = 0.5;
	// TEMOIN de l'A/B des quotas (s22) : rejoue la derivation s21 par classes
	// d'effets a la place de la derivation par les duaux du LP. Un drapeau le
	// temps d'une mesure — les deux derivations s'impriment dans tous les cas.
	bool quota_legacy = false;
	// TEMOIN de l'A/B des demandes transitoires (s23) : ne PAS compiler les
	// exigences --resolve/--summon-min dans le bilan matiere (comportement
	// s22quater : credit post-resolution seulement). Un drapeau le temps d'une
	// mesure — le cablage actif s'imprime dans tous les cas.
	bool resolve_legacy = false;
	// LES QUOTAS DU CHEMIN DANS LE LP (s24, chantier 4 — relaxation partielle
	// red-black, Katz-Hoffmann-Domshlak) : au raffinement, les usages
	// observes des hotes a quota entrent dans les capacites du LP — h et la
	// sous-echelle deviennent honnetes vis-a-vis de ce que le chemin a deja
	// depense. Arme aussi la colonne h_quota de la marche du theoreme 2 (le
	// juge mandate : 0 etat infaisable NOUVEAU le long de la reference).
	// Faux par defaut le temps de la mesure.
	bool quota_h = false;
	// GO-EXPLORE COMPLET, premiere moitie (s24) : les archives des recherches
	// du FINISSEUR (A1/A2/phase 2) entrent dans l'archive globale, chemins
	// re-enracines au depart. Jusqu'ici elles MOURAIENT avec leur phase — la
	// litterature (Go-Explore : « les decouvertes de chaque phase
	// renourrissent l'archive ») et la mesure (les lignes jointes naissent au
	// finisseur) disent la meme chose. Faux par defaut le temps de la mesure.
	bool archive_fin = false;
	// GO-EXPLORE COMPLET, seconde moitie (s24) : sous --rounds, l'archive
	// globale et la politique fusionnee PERSISTENT d'un round a l'autre, et
	// les workers de tirages du round suivant sont SEMES avec les cellules
	// portees. Sans lui, chaque round repart d'une archive vide et seule la
	// ligne jointe transite. Faux par defaut le temps de la mesure.
	bool carry = false;
	// DISCIPLINE (s22ter, demande operateur) : ne jamais proposer une
	// NEGATION du joueur sur son propre maillon de chaine (Crystal Wing,
	// Zalen, Silver Hound...). Famille de --no-activate/--no-chain — une
	// contrainte choisie, pas un elagage de qualite. Les effets vises se
	// derivent de la table declaree (categories NEGATE/DISABLE), zero nom.
	bool no_self_negate = false;
	// DISCIPLINE (s22quater, demande operateur) : tout le combo vit en MAIN
	// PHASE 1 — l'entree en Battle Phase (donc la Main 2) est retiree de
	// l'enumeration, « -> End Phase » reste.
	bool mp1_only = false;
	// Le domaine en TOURS (--turns, s22quater) : 0/1 = un tour (historique),
	// 2 = la ligne traverse le tour adverse (fenetres rapides seulement).
	uint64_t turns = 0;
	// L'ECHELLE AUTO-RAFFINANTE (s22, chantier 3) : re-serialisation depuis la
	// meilleure cellule-frontiere quand sp_max stagne depuis N tirages
	// mesures. 0 = eteint — un mecanisme est un drapeau le temps de le
	// mesurer.
	uint64_t refine_after = 0;
	// LA BOUCLE INTERNE (s23, directive operateur ; forme Go-Explore/ExIt —
	// docs/etat-de-lart-boucle-interne.md) : le budget --solve-ms se decoupe
	// en N rounds internes ; entre deux rounds, la meilleure ligne JOINTE
	// ecrite est reinjectee comme approche du suivant. « Une commande, un
	// resultat final » — la reinjection n'est plus le travail de l'operateur.
	// 1 = comportement historique (aucune banniere, aucun round).
	uint64_t rounds = 1;
	// Poids soustrait au logit d'un changement de phase. 0 = eteint (le temoin).
	double phase_w = 0.0;
	// CHANTIER 2 (session 19) : LE CHAINAGE ARRIERE COMME BIAIS.
	//
	// Poids ajoute au logit des choix qui JOUENT une carte dont la
	// decomposition a rebours exige la presence SUR LE TERRAIN — c'est-a-dire
	// l'hote d'une arete d'acquisition. `--assign-bias` designe des MATERIAUX et
	// mord sur les prompts de SELECTION ; celui-ci designe des OPERATEURS et
	// mord sur « que jouer ». Exige `--op-recipes` pour avoir de la matiere.
	//
	// REGLE 2, NON NEGOCIABLE : un plan est un BIAIS, jamais un elagage.
	double op_bias = 0.0;
	// Cartes OBSERVEES par la sonde, sans aucune contrainte (`--watch`).
	// Objection de l'operateur qui les a fait ecrire : `--resolve` est un
	// INDICE DEGUISE (biais d'indices d'office + gradient + exigence au but),
	// donc une sonde qui ne sait compter que des `--resolve` ne peut pas
	// mesurer « le solveur trouve-t-il SEUL ».
	std::vector<std::string> watch_specs;
	// BIAIS DERIVE DE LA CIBLE (session 16). Deux changements qui ne servent a
	// rien l'un sans l'autre, d'ou un seul drapeau :
	//   1. les codes du BOARD CIBLE entrent dans `hint_cards` — jusqu'ici seuls
	//      `--hint` (ecrit a la main) et `--resolve` y entraient, si bien que le
	//      solveur a qui l'on demande un Liger Dancer n'avait AUCUNE preference
	//      pour le coup « invoquer Liger Dancer » ;
	//   2. `MSG_SELECT_CARD` renseigne `Choice::card`, sans quoi le prompt qui
	//      decide QUELLE Fusion invoquer reste invisible au biais.
	// Ce n'est PAS de la connaissance metier : c'est lire l'enonce. C'est la
	// difference avec `--hint`, qui est une bequille.
	// --- SESSION 17 : LES QUATRE LEVIERS CONTRE LA LOI D'ARITE --------------
	// Tous eteints par defaut, tous separables, tous A/B-ables seuls. Voir
	// SearchConfig pour le raisonnement complet de chacun.
	//
	// (1) --assign : les prompts de sous-ensemble emettent EN PLUS les deux
	// sous-ensembles extremes au sens des recettes. Attaque la troncature
	// LEXICOGRAPHIQUE de l'enumeration, qui ne rend pas le bon sous-ensemble
	// rare mais ABSENT.
	bool assign = false;
	// (1bis) --assign-bias <f> : le mecanisme que le DIAGNOSTIC designe. Leo
	// Dancer est offert 14 433 fois dans le prompt « quel Lunalight envoyer au
	// cimetiere » et choisi 202 fois — 1,4 %. Ce poids oriente ce choix vers les
	// codes que le GRAPHE DE RECETTES designe comme materiaux.
	double assign_bias = 0.0;
	// (2) --hindsight <f> : chaque monstre d'extra deck reellement invoque
	// devient un but de substitution, et la meilleure ligne qui l'atteint subit
	// le gradient NRPA a f x alpha (HER, NeurIPS 2017).
	//
	// DEFAUT 0,5 DEPUIS LA SESSION 19. La valeur n'est pas neuve : c'est celle
	// que les deux etalons ont mesuree (x20,6 sur l'arite 3, separation complete
	// des supports a deux graines sur A ; `>=2` de 3/10 a 9/10 sur B). Elle
	// s'eteint par `--no-hindsight`.
	double hindsight = 0.5;
	size_t hindsight_k = 16;
	// (3) --recipe-w <f> : la distance de recettes dans le SCORE DES TIRAGES,
	// en progres. C'est le chantier que la session 16 a ecrit sans le brancher —
	// `RecipeDistance` n'existait que dans le finisseur.
	// (4) --backward : le nombre de sous-produits de la decomposition ET/OU deja
	// fabriques entre dans la PARTITION de la table de nouveaute (Serialized IW
	// sur la decomposition apprise, au lieu du but litteral).
	bool backward = false;
	// (5) --canonical-digest : confondre les COLONNES dans la cle de
	// transposition. Mesure d'attribution (session 17, points stables) : la
	// colonne vaut x33,7 de valeurs distinctes a elle seule, premier poste et de
	// loin, devant la charge utile du prompt (x1,38) et l'etat du processeur
	// (x1,00). Opt-in : les fleches de LIEN sont colonne-dependantes.
	// (6) --elide-forced : un prompt qui n'offre qu'UNE reponse legale est joue
	// en ligne — ni profondeur, ni entree de table, ni instantane d'arene. Ce
	// que l'attribution de la cle designe : la majorite des noeuds ne sont pas
	// des points de decision. Le finisseur le fait deja ; l'exhaustif, non.
	//
	// NON PROMU, ET LA RAISON EST UNE MESURE DE LA SESSION 19 : le drapeau
	// n'etait CABLE QUE dans `--growth`. Les +61 % de debit et le x2,1 de boards
	// de 9.24 (k) valent donc pour le chemin EXHAUSTIF, jamais pour la
	// recherche. Le cablage est corrige (c'est un correctif : le drapeau
	// pretendait agir) ; le DEFAUT, lui, reste eteint tant que le mecanisme n'a
	// pas ete juge la ou il agit desormais.
	bool elide_forced = false;
	// Restaure l'ordre HISTORIQUE des sous-ensembles (tailles croissantes),
	// pour attribuer le correctif C9. Un correctif dont on ne peut pas
	// eteindre l'effet n'est pas attribuable — il est seulement cru.
	// sqrt-LTS : re-enraciner le finisseur a chaque indice (chantier 10).
	bool levin_reroot = false;
	// sqrt-LTS-H (session 8, chantier 11 — arXiv:2605.30664 §3.2) : rerooter
	// HEURISTIQUE, doux, partout non nul. w_t = exp(-alpha * h(n_t)/h(racine)).
	// Contrairement a --reroot (rerooter DUR sur les indices), il ne demande
	// aucun evenement discret : dans un paysage plat ou l'indice ne tombe
	// jamais, c'est le seul des deux qui puisse decomposer. 0 = eteint.
	double reroot_h = 0.0;
	// MODE BUT SEUL (session 8).
	//  --no-plan : le repertoire de la reference est VIDE. C'est l'etalon B —
	//    un seul facteur change, et il chiffre ce que la reference valait.
	//  --target  : le board cible est construit de zero (au lieu d'etre capture
	//    sur la reference puis edite par une cascade de --board-remove).
	//  --no-ref  : le replay positionnel est degrade au rang de GABARIT de duel
	//    (en-tete, drapeaux, adversaire) ; sa ligne, son board et son repertoire
	//    sont tous ecartes. Implique --no-plan et exige --target.
	bool no_plan = false;
	bool no_ref = false;
	// Plafond de decisions d'une ligne cherchee. 0 = derive de la reference
	// (ref_decisions * 3/2 + 32), le comportement d'avant. A relever quand la
	// ligne VISEE est plus longue que la reference — viser trois Fusions quand
	// la reference n'en pose qu'une. Un plafond trop court tronque SANS LE DIRE.
	uint32_t max_decisions = 0;
	// Contraintes de ligne, brutes, resolues en codes une fois la base de
	// cartes chargee.
	std::vector<std::string> summon_specs;      // "5:Zalen|Crystal Wing"
	std::vector<std::string> guard_specs;       // "5:CW@terrain|Zalen@terrain+Junk Signal@main"
	std::vector<std::string> no_activate_specs; // "Assault Zone@terrain"
	// Cartes jamais CHAINEES par le joueur cible (--no-chain) : les gardes
	// (Zalen, Crystal Wing) repondent a une menace hypothetique — les chainer
	// sur nos propres activations est une branche inutile par construction.
	std::vector<std::string> no_chain_specs;
	std::string guard_off_spec;                 // "mainadv<=2"
	std::vector<std::string> resolve_specs;     // "PSY-Framelord Omega:2"
	// La ligne doit INVOQUER ces cartes (meme machinerie que --resolve, sur
	// les MSG_SUMMONING/SPSUMMONING) : "carte[:n]".
	std::vector<std::string> summon_min_specs;
	// Position de depart SYNTHETIQUE : decklist .ydk + main de depart, sans
	// replay de depart. La cible et les parametres de duel restent ceux de la
	// reference.
	std::string deck_file;                      // "D:\...\test 3.ydk"
	std::string hand_spec;                      // "carte|carte|..." (defaut :
												// la main de la reference)
	// Indices de domaine : cartes dont les coups recoivent une prime
	// d'echantillonnage NRPA.
	std::vector<std::string> hint_specs;
	// Approches des sessions passees (best_approach_*.yrp) servies au
	// finisseur comme racines supplementaires (chemin complet + reculs) :
	// l'archive Go-Explore qui persiste ENTRE les runs.
	std::vector<std::string> approach_files;
	// Cartes AJOUTEES a la main de l'adversaire du duel de depart (--opp-hand).
	// Donne un objet a la garde et au handrip quand le depart est un hand test
	// (adversaire sans main) ; des cartes JOUABLES (Nibiru...) sont necessaires
	// pour que le core ouvre des fenetres de reponse adverses.
	std::vector<std::string> opp_hand_specs;
	// Edition du board cible et contraintes de materiau.
	std::vector<std::string> board_add_specs;    // "Naturia Beast[@ATK|DEF]"
	std::vector<std::string> board_remove_specs; // "Hot Red Dragon..."
	// Board cible construit DE ZERO (--target, meme grammaire que --board-add).
	std::vector<std::string> target_specs;
	std::vector<std::string> material_specs;     // "Chaos Angel:lumiere"
};

// n-ieme invocation (1-base) -> codes canoniques admis.
using SummonConstraints = std::map<uint32_t, std::vector<uint32_t>>;

// L'ensemble des contraintes de ligne, resolues en codes.
struct LineConstraints {
	SummonConstraints summons;
	uint32_t guard_after = 0;
	std::vector<GuardClause> guard;
	// Extinction de la garde : plus exigee quand la main adverse compte au
	// plus ce nombre de cartes (-1 = jamais). Un deck handrip eteint la menace.
	int guard_opp_hand_release = -1;
	std::map<uint32_t, uint32_t> no_activate;   // code -> masque LOCATION_
	// Cartes jamais chainees par le joueur cible (--no-chain, codes
	// canoniques) : elaguees a l'ENUMERATION des fenetres de chaine.
	std::vector<uint32_t> no_chain;
	// Effets de NEGATION du deck (--no-self-negate) : paires (code canonique,
	// desc ; desc 0 = toute la carte), derivees de la table declaree apres le
	// chargement des scripts. Jamais proposes sur un maillon A NOUS.
	std::vector<std::pair<uint32_t, uint64_t>> self_negate;
	// Minimum de resolutions d'effet, filtre par zone d'ACTIVATION (--resolve
	// "carte[@zone][:n]" ; cf. ResolveReq — l'effet de cimetiere d'Omega ne
	// compte pas pour le handrip, faux positif mesure).
	std::vector<ResolveReq> resolve_min;
	// Indices de domaine (--hint) : pas des contraintes, un prior — ils
	// n'entrent pas dans Any() et ne gatent rien.
	std::vector<uint32_t> hints;
	// Cartes ajoutees a la main ADVERSE du duel de depart (--opp-hand). Pas une
	// contrainte de ligne (hors de Any()) : un modificateur de position de
	// depart, qui voyage avec le reste de la configuration.
	std::vector<uint32_t> opp_hand;
	// Contrainte de materiau : (carte canonique, masque d'attributs) — au
	// moins un materiau de l'invocation doit porter un de ces attributs.
	std::vector<std::pair<uint32_t, uint32_t>> material_req;
	// Edition du board CIBLE (ce n'est pas une contrainte de ligne) : cartes
	// ajoutees (code canonique, position) et retirees. Exige la
	// transplantation (--deck ou --start) : en reparation, la reference ne
	// peut plus servir de controle sur une cible qu'elle n'atteint pas.
	std::vector<std::pair<uint32_t, uint32_t>> board_add;
	std::vector<uint32_t> board_remove;
	// --target : le board cible ne part PAS de la capture de la reference mais
	// d'une table vide. `board_add` porte alors la cible entiere.
	bool target_scratch = false;
	bool AnyBoardEdit() const {
		return !board_add.empty() || !board_remove.empty() || target_scratch;
	}
	bool Any() const {
		return !summons.empty() || !guard.empty() || !no_activate.empty() ||
			   !no_chain.empty() || !resolve_min.empty() ||
			   !material_req.empty();
	}
};

// --summon-min DERIVE du comptage, au lieu d'ecrit a la main.
//
// Un `--summon-min "Liger Dancer:3"` tape par l'operateur est une connaissance
// de domaine ; derive du board cible, c'est une CONSEQUENCE du but. Trois
// gardes non negociables :
//   - le plafond de quatre entrees de --resolve/--summon-min est respecte ;
//   - seules les cartes d'EXTRA DECK sont retenues. Une carte de main deck peut
//     arriver par des voies qui ne sont pas des invocations, et les pieges 28
//     et 31 disent que --resolve ne vaut que pour des EVENEMENTS RARES : une
//     contrainte posee sur un evenement frequent effondre la politique ;
//   - les entrees ecrites a la main l'emportent : on complete, on ne remplace pas.
//
// `cons` est LU seulement (pour ne pas doubler une entree manuelle) ; l'ecriture
// se fait dans `cfg`, c'est-a-dire dans le GUIDE DE RECHERCHE. Ce n'est pas un
// contournement du controle « verifie avant ecriture » : une contrainte DERIVEE
// du board cible est REDONDANTE a la verification, et pour une raison exacte —
// si le board final porte trois Liger Dancer, alors trois invocations Fusion ont
// necessairement eu lieu, une invocation ne posant qu'une carte. Le
// verificateur, qui compare le board final a la cible, l'a donc deja verifiee en
// verifiant le board. Elle sert a GUIDER plus tot, pas a JUGER plus tard.
void DeriveSummonMin(const std::vector<TargetCount>& counts,
					 const LineConstraints& cons, const CardDB& db,
					 SearchConfig& cfg) {
	constexpr size_t kMaxEntries = 4;
	std::vector<ResolveReq> eff = cons.resolve_min;   // les manuelles d'abord
	size_t added = 0;
	for(const TargetCount& t : counts) {
		if(eff.size() >= kMaxEntries)
			break;
		if(t.token || !FromExtraDeck(t.mech))
			continue;
		bool already = false;
		for(const ResolveReq& r : eff)
			if(r.code == t.code) { already = true; break; }
		if(already)
			continue;
		ResolveReq r;
		r.code = t.code;
		r.min_count = t.need;
		r.on_summon = true;
		eff.push_back(r);
		++added;
	}
	if(!added) {
		std::printf("  --derive-summon-min : rien a deriver (aucune carte "
					"d'extra deck libre sous le plafond de %zu).\n", kMaxEntries);
		return;
	}
	cfg.resolve_min = eff;
	std::printf("  --derive-summon-min : %zu contrainte(s) DERIVEE(S) du board "
				"cible.\n     Guide de recherche ; redondantes a la "
				"verification, qui juge le board lui-meme :\n", added);
	for(const ResolveReq& r : eff)
		std::printf("      %ux %s\n", r.min_count, db.Name(r.code).c_str());
	if(counts.size() > kMaxEntries)
		std::printf("      (plafond de %zu entrees : les cartes cibles au-dela "
					"ne sont pas contraintes)\n", kMaxEntries);
}


void Usage() {
	std::printf(
		"usage: combosolver <replay.yrpX> [options]\n"
		"\n"
		"  --workdir <dir>    installation EDOPro (defaut D:\\ProjectIgnis)\n"
		"  --scriptdir <dir>  jeu de scripts prioritaire (repetable)\n"
		"                     A utiliser avec un export du depot contemporain du\n"
		"                     replay : un jeu decale fait diverger le rejeu en\n"
		"                     silence (docs/combo-solver-design.md 6bis).\n"
		"  --player <0|1>     joueur dont on optimise le tour (defaut 0)\n"
		"  --arena-mb <n>     espace d'adressage reserve a l'arene (defaut 256)\n"
		"  --no-arena         allocateur systeme, sans instantane (comparaison)\n"
		"  --keep-gc          laisse tourner le ramasse-miettes Lua (comparaison)\n"
		"  --growth           mesure la croissance du graphe d'etats (jalon 0b)\n"
		"  --growth-max <n>   profondeur maximale exploree (defaut 14)\n"
		"  --growth-ms <ms>   budget temps par profondeur (defaut 20000)\n"
		"  --start <replay>   refaire le board de la reference depuis CE duel-la\n"
		"                     (autre deck, autre main, autre graine). Implique\n"
		"                     --solve.\n"
		"  --deck <f.ydk>     refaire le board de la reference depuis CETTE\n"
		"                     decklist, sans replay de depart. La main de depart\n"
		"                     se donne par --hand (defaut : celle de la\n"
		"                     reference). Implique --solve.\n"
		"  --hand <cartes>    main de depart du combo, cartes separees par '|'\n"
		"                     (codes ou fragments de noms). Avec --deck.\n"
		"  --hint <carte>     indice de domaine : les coups qui engagent cette\n"
		"                     carte (invoquer, activer, positionner) recoivent\n"
		"                     une prime d'echantillonnage NRPA. Repetable.\n"
		"                     Ex : --hint \"Hot Red Dragon Archfiend Abyss\"\n"
		"  --opp-hand <c>     AJOUTE ces cartes a la main de l'adversaire du\n"
		"                     duel de depart (cartes separees par '|',\n"
		"                     repetable). Donne un objet a la garde et au\n"
		"                     handrip sur un depart hand test ; il faut des\n"
		"                     cartes JOUABLES (Nibiru...) pour que des fenetres\n"
		"                     adverses s'ouvrent. Les replays produits ne se\n"
		"                     rejouent qu'avec le meme --opp-hand.\n"
		"  --board-add <c>    EDITE le board cible : exige cette carte en plus\n"
		"                     (<c> = carte[@ATK|DEF], defaut ATK). Repetable.\n"
		"                     Exige --deck ou --start.\n"
		"  --board-remove <c> EDITE le board cible : n'exige plus cette carte.\n"
		"                     Repetable. Exige --deck ou --start.\n"
		"  --target <c>       POSE le board cible de zero (meme grammaire que\n"
		"                     --board-add). La capture de la reference n'entre\n"
		"                     pas : plus de cascade de --board-remove. Repetable.\n"
		"  --no-plan          le REPERTOIRE de la reference est ecarte : la\n"
		"                     politique NRPA demarre uniforme. Mesure ce que la\n"
		"                     reference valait (etalon B du mode but seul).\n"
		"  --no-ref           MODE BUT SEUL : le replay positionnel n'est plus\n"
		"                     qu'un gabarit de duel (drapeaux, LP, adversaire).\n"
		"                     Implique --no-plan ; exige --target et --deck.\n"
		"  --material <spec>  l'invocation de cette carte doit consommer au\n"
		"                     moins un materiau de ces attributs. <spec> =\n"
		"                     carte:attr[,attr...], attributs : lumiere tenebres\n"
		"                     terre eau feu vent divin. Repetable.\n"
		"                     Ex : --material \"Chaos Angel:lumiere\"\n"
		"  --outdir <dir>     ou ecrire les replays produits (defaut solutions/)\n"
		"  --solve            recherche guidee vers le board cible\n"
		"  --solve-ms <ms>    budget temps de la recherche (defaut 120000)\n"
		"  --threads <n>      workers de recherche (defaut : tous les coeurs)\n"
		"  --max-rollouts <n> MODE DETERMINISTE (audit 18) : budget en TIRAGES\n"
		"                     par worker au lieu du temps de mur. Le budget en\n"
		"                     millisecondes est la CAUSE du non-determinisme —\n"
		"                     deux runs --threads 1 a la meme graine font 41 232\n"
		"                     et 42 179 tirages, donc ne s'arretent pas au meme\n"
		"                     point de la trajectoire NRPA. Avec --threads 1,\n"
		"                     deux executions font exactement le meme travail.\n"
		"                     Cout mesure du mono-worker : /5,1 a /5,9.\n"
		"  --max-nodes <n>    idem, en NOEUDS developpes par worker.\n"
		"  --no-adapt-to-peak DEFAUT DEPUIS LA s19 : n'adapter que le PREFIXE qui\n"
		"                     a produit le score. Le score d'un tirage est un MAX\n"
		"                     sur les prefixes, mais AdaptRun renforcait TOUS les\n"
		"                     pas : une ligne qui culmine au pas 200 puis erre 230\n"
		"                     pas apprenait l'effondrement aussi fort que la\n"
		"                     montee. x2,6 sur l'arite 3 (deux paires, etalon A),\n"
		"                     et >=2 de 3/10 a 9/10 sur B. Ce drapeau l'ETEINT,\n"
		"                     pour rejouer l'A/B.\n"
		"  --max-decisions <n>  plafond de profondeur des tirages, en decisions.\n"
		"                     Defaut : derive de la reference (1,5x + 32).\n"
		"  --elide-forced     un prompt a REPONSE UNIQUE est joue en ligne, avant\n"
		"                     la table : ni entree, ni instantane, ni profondeur,\n"
		"                     ni evaluation. 74,6 %% des noeuds n'offrent aucun\n"
		"                     choix ; +61 %% de debit et x2,1 de boards (9.24 (k)).\n"
		"                     La comptabilite d'actions, de tours, d'invocations\n"
		"                     et de resolutions est conservee.\n"
		"                     ATTENTION s19 : jusqu'ici il n'etait CABLE QUE dans\n"
		"                     --growth. Les chiffres ci-dessus valent donc pour le\n"
		"                     chemin EXHAUSTIF ; sur la RECHERCHE il etait inerte\n"
		"                     (preuve deterministe : 3000 tirages / 149334 etats /\n"
		"                     3002 adaptations a l'octet pres avec et sans). Le\n"
		"                     cablage est corrige, le mecanisme reste A JUGER la.\n"
		"  --assign-bias <f>  poids d'echantillonnage des coups qui engagent un\n"
		"                     code que le graphe de RECETTES designe comme\n"
		"                     MATERIAU. Allume --card-on-select. Sur l'etalon A :\n"
		"                     Leo Dancer au cimetiere 56 -> 1 537 (x27), chaine\n"
		"                     causale verifiee (9.24 (n)).\n"
		"  --dive-full        finisseur : empiler un niveau d'arene a chaque\n"
		"                     plongee (A/B de --no-dive-full).\n"
		"  --merged-pop       finisseur : depilage fusionne (A/B de\n"
		"                     --no-merged-pop).\n"
		"  --width            mesure la largeur effective (atomes IW) le long\n"
		"                     de la ligne de reference, sans recherche\n"
		"  --novelty <n>      patience de l'elagage par nouveaute (defaut :\n"
		"                     auto-calibree par la mesure de largeur)\n"
		"  --no-novelty       desactive l'elagage par nouveaute\n"
		"  --no-nrpa          tirages gloutons seuls, sans politique apprise\n"
		"  --seed <n>         graine des tirages (defaut : derivee du temps et\n"
		"                     imprimee — la redonner rejoue les memes tirages)\n"
		"  --nrpa-bias <x>    biais GNRPA des coups au repertoire (defaut 1.5)\n"
		"  --nrpa-keep <x>    persistance de la politique NRPA au redemarrage :\n"
		"                     poids attenues par x au lieu de repartir de zero\n"
		"                     (defaut 0.5 ; 0 = politique vierge)\n"
		"  --nrpa-alpha <x>   pas d'adaptation NRPA (defaut 1.0). Petit = la\n"
		"                     politique se deplace LENTEMENT — la moitie de la\n"
		"                     recette Montparnasse (l'autre est --nrpa-iters)\n"
		"  --nrpa-iters <n>   iterations par niveau NRPA (defaut 24). Le cout d'un\n"
		"                     appel de niveau L est n^L tirages : monter n allonge\n"
		"                     l'exploration d'un meme bassin avant de rendre la main\n"
		"  --finisher <mode>  finisseur de la transplantation : levin (archive\n"
		"                     Go-Explore + recul + Levin Tree Search sur la\n"
		"                     politique NRPA, defaut), mono (l'ancien : le seul\n"
		"                     meilleur etat), ab (les deux a budget egal)\n"
		"  --archive-k <n>    taille de l'archive d'etats du finisseur (defaut 16)\n"
		"  --finisher-min <ms> budget minimal RESERVE au finisseur (0 = repartition\n"
		"                     d'origine). Pour les runs de CONVERSION ou --approach\n"
		"                     fournit deja les racines.\n"
		"  --levin-h <x>      poids PHS* de la distance au but (cartes +\n"
		"                     resolutions manquantes) dans le cout du finisseur\n"
		"                     (defaut 1.0 ; 0 = Levin pur, aveugle au but)\n"
		"  --no-dive-full     finisseur : ne plus empiler un niveau d'arene a\n"
		"                     chaque noeud de chaine rejoue (retour au rejeu\n"
		"                     d'avant session 12 — bras temoin d'A/B)\n"
		"  --lifo-ties        finisseur : a cout de Levin EGAL, extraire le\n"
		"                     noeud enfile en dernier (refute seul, eteint par\n"
		"                     defaut — bras d'A/B)\n"
		"  --resolve-weight <x> poids d'une resolution exigee dans le gradient\n"
		"                     des tirages (defaut 250 ; 100 = une carte cible)\n"
		"  --optimize         OPTIMISATION DE COUT anytime : la recherche ne\n"
		"                     s'arrete plus a la premiere solution (chaque\n"
		"                     solution resserre la borne), le score de but NRPA\n"
		"                     devient lexicographique (brulees, puis actions,\n"
		"                     puis decisions), les tirages continuent APRES le\n"
		"                     but (les recuperations reduisent les brulees), et\n"
		"                     le finisseur tourne meme quand des lignes existent\n"
		"  --burn-slack <n>   marge de la borne brulees B&B (defaut 6) : coupe\n"
		"                     les etats a plus de meilleures_brulees + n (les\n"
		"                     brulees ne sont PAS monotones — recuperations ;\n"
		"                     la marge se mesure sur la reference). 255 = off\n"
		"  --burn-limit <n>   graine de la borne : meilleures brulees connues\n"
		"                     d'avance (0 = aucune)\n"
		"  --no-burn-share    ne PAS partager la borne brulees entre workers\n"
		"                     (defaut : partagee — un worker qui ameliore coupe\n"
		"                     chez tous). Sert a l'A/B.\n"
		"  --prior <f|dir>    prior par rejeu de solutions : les plan_key des\n"
		"                     lignes donnees (fichier .yrp ou dossier, repetable)\n"
		"                     sont releves chacune sur SON duel et deviennent des\n"
		"                     poids INITIAUX de politique NRPA — une politique\n"
		"                     qui sait deja ripper. Tirages ET fenetres --fire.\n"
		"  --prior-weight <x> poids d'un coup present dans tout le corpus\n"
		"                     (defaut 2.0 ; proportionnel a sa frequence sinon)\n"
		"  --adapt <f|dir>    rejeu d'ADAPTATION du corpus (repetable) : les\n"
		"                     lignes donnees sont relevees en SEQUENCES DE\n"
		"                     DECISIONS (choix legaux + choisi) et adaptees dans\n"
		"                     la politique NRPA avant le premier tirage. Signal\n"
		"                     discriminatif la ou --prior ne donne qu'une prime\n"
		"                     par coup (mesure NEUTRE, session 6).\n"
		"  --adapt-passes <n> passes d'adaptation par ligne (defaut 4 ; 0 coupe\n"
		"                     le mecanisme sans toucher au releve — c'est l'A/B)\n"
		"  --options <n>      OPTIONS (chantier 17) : catalogue de n macros\n"
		"                     minees dans le corpus --adapt et proposees comme\n"
		"                     UNE unite d'echantillonnage aux tirages NRPA\n"
		"                     (0 = eteint, defaut). La prevision ne justifie que\n"
		"                     le gros catalogue : 256.\n"
		"  --options-support <n>  occurrences minimales d'une macro (defaut 2)\n"
		"  --options-len <n>  longueur maximale d'une macro (defaut 8)\n"
		"  --options-window <n>  ne proposer une macro qu'a +/- n decisions de\n"
		"                     sa position d'origine dans le corpus (defaut 0 =\n"
		"                     pas de garde). REFUTEE (9.19 (g)) ; pour l'A/B.\n"
		"  --options-ctx <n>  garde SEMANTIQUE : ne proposer une macro que si\n"
		"                     le contexte courant est compatible avec une\n"
		"                     occurrence du corpus (cartes cibles posees\n"
		"                     exactes, main a +/- n). -1 = eteinte (defaut) ;\n"
		"                     15 = ne garder que les cartes posees.\n"
		"  --options-online <s>  MINAGE EN LIGNE : re-miner le catalogue toutes\n"
		"                     les s secondes sur les meilleures lignes DU RUN\n"
		"                     (0 = eteint, defaut). Aucun corpus externe requis :\n"
		"                     le run part nu et s'arme lui-meme. Implique\n"
		"                     --options 256 si --options n'est pas donne.\n"
		"  --options-pool <n> corpus vivant : lignes retenues au total (defaut 12)\n"
		"  --options-per-worker <n>  et au plus n par worker (defaut 2) — c'est\n"
		"                     la POMPE A DIVERSITE : sans quota, les seize workers\n"
		"                     versent seize fois la meme meilleure ligne partagee.\n"
		"  --no-merged-pop    finisseur : revenir au depilage niveau par niveau\n"
		"                     (temoin d'A/B ; le depilage fusionne est le\n"
		"                     defaut — chaque page chaude recopiee une fois)\n"
		"  --finisher-post-goal  finisseur : sous --optimize, un noeud-but\n"
		"                     CONTINUE (recuperation d'apres-but, piege 35) au\n"
		"                     lieu de s'arreter. Opt-in, a juger sur A/B.\n"
		"  --finisher-options  finisseur : les macros du catalogue deviennent\n"
		"                     des ARETES de l'arbre de Levin (cout log 1/pi,\n"
		"                     avance de k decisions, avortement = arete morte).\n"
		"                     Opt-in. ATTENTION : allume, il change LEGITIMEMENT\n"
		"                     les comptes d'expansions — le controle devient\n"
		"                     memes best par racine / aucune solution perdue /\n"
		"                     EPUISE toujours EPUISE.\n"
		"  --nrpa-temp <t>    temperature du softmax des tirages (defaut 1.0).\n"
		"                     t < 1 concentre la masse sur les coups les mieux\n"
		"                     classes SANS changer le classement — le seul\n"
		"                     levier de masse connu (GNRPA 2003.10024).\n"
		"  --nrpa-level <n>   niveau d'imbrication NRPA, 1..4. Defaut : 3 si le\n"
		"                     budget des tirages depasse 180 s, 2 sinon — un\n"
		"                     seuil qui change l'ALGORITHME (~576 tirages par\n"
		"                     appel de niveau contre ~13 824) et qui separait\n"
		"                     les commandes de plusieurs A/B publies. Le niveau\n"
		"                     effectif est desormais imprime dans tous les cas.\n"
		"  --recipes <w>      GRAPHE DE RECETTES (chantier 16) : h devient la\n"
		"                     distance en INVOCATIONS restantes sur les recettes\n"
		"                     OBSERVEES, materiaux intermediaires compris — une\n"
		"                     distance qui DECROIT la ou le h plat ne bouge pas.\n"
		"                     w = 0 : le graphe est alimente et MESURE sans\n"
		"                     entrer dans le cout (chiffrer avant de decider).\n"
		"                     w > 0 : il pese dans h. Il ne PRUNE jamais : une\n"
		"                     recette inconnue vaut 1, donc au pire h redevient\n"
		"                     le h plat d'aujourd'hui.\n"
		"  --landmarks <f|d>  GRAPHE DE LANDMARKS APPRIS (chantier 18,\n"
		"                     arXiv:2508.21564) : apprend, depuis des plans\n"
		"                     RESOLUS, les faits (carte, zone, COMPTE) que tout\n"
		"                     plan atteint, dans quel ordre, et combien de fois\n"
		"                     — les BOUCLES DE REPETITION du papier. Repetable.\n"
		"                     Le graphe est imprime : ce qu'il a appris se lit\n"
		"                     AVANT de le laisser peser.\n"
		"  --landmark-w <f>   poids d'un accomplissement dans le SCORE DES\n"
		"                     TIRAGES, en unites de materiel (une carte cible\n"
		"                     posee vaut 100). C'est le branchement qui compte :\n"
		"                     poser « Leo Dancer au cimetiere » une 2e fois y\n"
		"                     fait monter le score AVANT qu'aucune cible ne\n"
		"                     soit sur le terrain. 0 = appris et mesure sans\n"
		"                     peser.\n"
		"  --landmark-h <f>   poids du h de landmarks dans le FINISSEUR (meme\n"
		"                     point d'entree que --recipes). Separe de\n"
		"                     --landmark-w pour qu'un A/B n'en bouge qu'un.\n"
		"  --assign           (1) ASSIGNATION RESOLUE (session 17,\n"
		"                     arXiv:2010.12001). Les prompts de sous-ensemble\n"
		"                     emettent EN PLUS les deux sous-ensembles extremes\n"
		"                     au sens des recettes. La troncature de\n"
		"                     l'enumeration est LEXICOGRAPHIQUE : au-dela de\n"
		"                     --max-subsets le bon sous-ensemble n'est pas rare,\n"
		"                     il est ABSENT, et aucun poids ne rattrape cela.\n"
		"  --hindsight <f>    (2) HINDSIGHT (HER, NeurIPS 2017). Chaque monstre\n"
		"                     d'extra deck REELLEMENT invoque devient un but de\n"
		"                     substitution, et la meilleure ligne qui l'atteint\n"
		"                     subit le gradient NRPA a f x alpha. Le solveur pose\n"
		"                     deja des milliers de Fusions bon marche par run et\n"
		"                     jette tout : le signal existe, il n'est pas lu.\n"
		"                     DEFAUT 0,5 DEPUIS LA s19 (x20,6 sur l'arite 3 et\n"
		"                     separation complete des supports sur A ; >=2 de\n"
		"                     3/10 a 9/10 sur B). --no-hindsight l'eteint.\n"
		"  --hindsight-k <n>  buts de substitution retenus au plus (defaut 16).\n"
		"  --backward         (4) SERIALISATION A REBOURS (Retro*, AO*). Une\n"
		"                     invocation est un noeud ET : l'arite, fatale en\n"
		"                     avant, devient une DECOMPOSITION en arriere. Le\n"
		"                     nombre de sous-produits deja fabriques entre dans\n"
		"                     la partition de la table de nouveaute, qui se\n"
		"                     rouvre donc AVANT qu'aucune cible ne soit posee.\n"
		"  --watch <carte>    carte OBSERVEE par --probe-repeat, SANS aucune\n"
		"                     contrainte, aucun gradient, aucun biais d'indice.\n"
		"                     A utiliser des qu'on mesure « le solveur\n"
		"                     trouve-t-il SEUL » : --resolve, lui, est un\n"
		"                     indice deguise (il recoit hint_bias d'office).\n"
		"                     Repetable, au plus 4.\n"
		"  --probe-repeat     SONDE DE REPETITION (session 16) : par carte\n"
		"                     --summon-min/--resolve, l'histogramme des\n"
		"                     invocations PAR TIRAGE, et — a la PREMIERE — la\n"
		"                     distance de recettes a un exemplaire DE PLUS,\n"
		"                     comparee a la meme distance depuis l'etat de\n"
		"                     depart. Separe les deux pannes que best_overlap\n"
		"                     confond : le 2e exemplaire JAMAIS TENTE (le\n"
		"                     materiau etait la — panne d'echantillonnage) du 2e\n"
		"                     TOUJOURS PERDU (la chaine etait consommee — panne\n"
		"                     de h). Implique --recipes 0.\n"
		"  --operators        HARNAIS D'OPERATEURS DECLARES (session 19) :\n"
		"                     extrait des SCRIPTS LUA du deck la table des\n"
		"                     operateurs — preconditions (SetRange,\n"
		"                     SetCountLimit), produit (SetOperationInfo :\n"
		"                     categorie ET zone), ETAT ACCORDE (EFFECT_ADD_CODE,\n"
		"                     EFFECT_EXTRA_FUSION_MATERIAL...), recettes\n"
		"                     (Fusion.AddProcMix*) —, l'imprime, puis la\n"
		"                     CONFRONTE au plan rejoue : chaque activation\n"
		"                     correspond-elle a un operateur declare, et la\n"
		"                     sequence est-elle valide sous les preconditions ?\n"
		"                     Les constantes viennent du `constant.lua` du jeu :\n"
		"                     aucune carte n'est nommee dans le code. Instrument,\n"
		"                     pas mecanisme — il ne change pas la recherche.\n"
		"  --op-recipes       amorce le graphe de recettes depuis les OPERATEURS\n"
		"                     DECLARES (session 19, chantier 1) au lieu du seul\n"
		"                     texte anglais : recettes en CODES\n"
		"                     (Fusion.AddProcMix*), et surtout le type de nœud\n"
		"                     qui manquait — « ce CODE peut etre ACQUIS », pour\n"
		"                     chaque EFFECT_ADD_CODE / EFFECT_CHANGE_CODE du\n"
		"                     deck. Le graphe rangeait la carte a code emprunte\n"
		"                     comme un PRODUIT A FABRIQUER, d'ou l'echec de\n"
		"                     --backward (9.24 (e)). Les aretes posees sont\n"
		"                     IMPRIMEES une par une. Exige --recipes.\n"
		"  --op-bias <f>      poids ajoute aux coups qui JOUENT une carte que la\n"
		"                     decomposition a rebours exige SUR LE TERRAIN — les\n"
		"                     hotes des aretes d'acquisition. --assign-bias\n"
		"                     designe des MATERIAUX et mord sur les prompts de\n"
		"                     SELECTION ; celui-ci designe des OPERATEURS et mord\n"
		"                     sur « que jouer ». Un plan est un BIAIS, jamais un\n"
		"                     elagage : rien n'est retire de l'espace. Exige\n"
		"                     --recipes et --op-recipes ; sa VIE est imprimee\n"
		"                     (proposees / prises) et a zero il est INERTE.\n"
		"  --no-seed-recipes  n'amorce PAS le graphe avec le texte de carte : le\n"
		"                     graphe n'apprend plus que des invocations reussies.\n"
		"  --no-seed-quant    amorce les seuls materiaux NOMMES, sans les\n"
		"                     exigences CARDINALES (« 3 \"Lunalight\" monsters »,\n"
		"                     « 2 Level 4 monsters »). Sur l'etalon A, huit cartes\n"
		"                     de l'extra sur dix ne nomment aucune carte : ce\n"
		"                     drapeau isole ce que les cardinales apportent.\n"
		"  --derive-summon-min\n"
		"                     derive les contraintes --summon-min du BOARD CIBLE\n"
		"                     au lieu de les ecrire a la main : 3x Liger Dancer\n"
		"                     = trois EVENEMENTS d'invocation Fusion (jamais\n"
		"                     « trois Polymerisations » : le declencheur varie).\n"
		"                     Le comptage est TOUJOURS imprime ; ce drapeau le\n"
		"                     branche sur les contraintes.\n"
		"  --hint-bias <b>    poids du biais d'INDICE dans l'echantillonnage\n"
		"                     (defaut 2.0). C'est le canal par lequel la\n"
		"                     connaissance du joueur entre : les cartes\n"
		"                     --resolve/--summon-min le recoivent d'office.\n"
		"                     Son voisin --nrpa-bias existait, pas lui.\n"
		"  --max-subsets <n>  sous-ensembles emis par prompt de selection\n"
		"                     (defaut 24). C'est le plafond du facteur de\n"
		"                     branchement de tous les SELECT_CARD/SELECT_SUM ;\n"
		"                     les tailles sont visitees en alternant depuis les\n"
		"                     deux bouts (min, max, min+1...). Les troncatures\n"
		"                     sont comptees en colonne « sous-ens. ».\n"
		"  --reroot           sqrt-LTS a rerooter DUR (2412.05196) : le finisseur\n"
		"                     se re-enracine a chaque INDICE (le nombre de cartes\n"
		"                     cibles posees change). Sans indice, inerte.\n"
		"  --reroot-h <a>     sqrt-LTS-H a rerooter HEURISTIQUE (2605.30664 §3.2) :\n"
		"                     poids exp(-a*h/h0) sur CHAQUE noeud, donc actif meme\n"
		"                     quand aucun indice ne tombe. a = temperature\n"
		"                     inverse (0 = eteint). Exclusif avec --reroot.\n"
		"  --ctx-shrink <k>   politique a DEUX NIVEAUX : un poids par coup ET un\n"
		"                     poids par (coup, contexte), melanges en convexe\n"
		"                     s = n/(n+k) ou n est l'evidence de la case\n"
		"                     contextuelle. Le contexte est le nombre de cartes\n"
		"                     du board cible deja posees. k negatif (defaut) =\n"
		"                     eteint, comportement d'avant. La courbe d'accord\n"
		"                     imprimee par --adapt calibre k sans depenser un run.\n"
		"  --qhat <k>         BANDIT DE TETE A STATISTIQUE DE PERMUTATION\n"
		"                     (MCPS 2510.06381, pour de vrai) : sur les k\n"
		"                     premieres decisions du tirage, le coup est choisi\n"
		"                     par argmax de (n Q + n^ Q^)/(n + n^) — Q, moyenne\n"
		"                     des recompenses des tirages passes par ce noeud\n"
		"                     puis par ce coup ; Q^, moyenne sur TOUS les\n"
		"                     tirages contenant ce coup ET ceux du chemin, dans\n"
		"                     n'importe quel ordre. Poids proportionnels aux\n"
		"                     effectifs : aucun hyperparametre de biais. Au-dela\n"
		"                     de k, NRPA echantillonne comme avant. Ces k\n"
		"                     decisions sont EXCLUES du gradient NRPA (elles ne\n"
		"                     sont pas tirees du softmax). 0 = eteint.\n"
		"  --qhat-window <W>  taille de la fenetre glissante de tirages, PAR\n"
		"                     worker (defaut 4096 ; le papier prend 10000, que\n"
		"                     nous paierions seize fois). La memoire mesuree est\n"
		"                     imprimee au bilan.\n"
		"  --qhat-rho <r>     visites au bout desquelles un noeud non racine\n"
		"                     GELE sa statistique de permutation et devient la\n"
		"                     reference de son sous-arbre (defaut 32).\n"
		"  --qhat-nodes <n>   plafond de noeuds du bandit par worker (65536).\n"
		"  --no-qhat-probe    ne pas imprimer la sonde de la premiere decision.\n"
		"  --reenter <p>      RETOUR AU BARREAU (s21) : probabilite qu'un tirage\n"
		"                     NRPA reparte d'une cellule d'archive (un palier de\n"
		"                     l'echelle x*) au lieu de la racine — la moitie de\n"
		"                     SIW_R qui manquait aux tirages. Ne mord que sous\n"
		"                     serialisation armee. Defaut 0.5 ; 0 = temoin A/B.\n"
		"  --canonical-zones  n'explorer qu'une zone libre representative par\n"
		"                     type de zone. Declare depuis longtemps, allume\n"
		"                     nulle part jusqu'a la s15. Les fleches de lien et\n"
		"                     les colonnes peuvent tout changer : a juger.\n"
		"  --target-subset    le board final doit CONTENIR la cible au lieu de\n"
		"                     lui etre EGAL. C'est le DEFAUT des que la cible\n"
		"                     est POSEE (--target) depuis la session 15.\n"
		"  --target-exact     restaure l'EGALITE exacte sur une cible posee\n"
		"                     (ancien defaut, conserve pour l'A/B). Attention :\n"
		"                     il EXIGE une zone S/T vide, ce qui est\n"
		"                     insatisfiable des que la main porte une magie\n"
		"                     continue.\n"
		"                     lui etre EGAL. Par defaut le but est l'egalite\n"
		"                     EXACTE, zone S/T comprise : correct pour une\n"
		"                     cible CAPTUREE sur une vraie ligne, INSATISFIABLE\n"
		"                     pour une cible POSEE par --target, dont la zone\n"
		"                     S/T est VIDE alors qu'une magie continue de la\n"
		"                     main (Tenki) y reste des qu'on l'active.\n"
		"  --ctx-max <n>      plafond d'entrees du niveau contextuel par worker\n"
		"                     (defaut 262144, 0 = illimite). Au plafond, les\n"
		"                     cases existantes vivent, aucune neuve n'est creee.\n"
		"  --fire <carte>     TEST ADVERSE : ajoute la carte a la main adverse\n"
		"                     et la fait JOUER a chaque fenetre ou elle est\n"
		"                     legale (un essai par fenetre) ; la recherche doit\n"
		"                     refermer le board depuis l'etat post-injection.\n"
		"                     La garde statique devient une preuve dynamique.\n"
		"                     Les replays produits ne se rejouent qu'avec\n"
		"                     --opp-hand <carte> en mode juge.\n"
		"  --fire-spare <c>   carte SACRIFIABLE pour contrer : le board cible\n"
		"                     sans elle est aussi accepte au but (contrer par\n"
		"                     Zalen consomme Junk Signal)\n"
		"  --fire-ms <ms>     budget de recherche par fenetre (defaut 45000)\n"
		"  --fire-bake        cuit la carte tiree dans l'en-tete des replays\n"
		"                     produits (deck adverse, servie en main par le\n"
		"                     pseudo-melange) : ils se rejouent depuis leur\n"
		"                     fichier — EDOPro les VISIONNE sans drapeau. La\n"
		"                     carte remplace la derniere carte de la main\n"
		"                     adverse d'origine (start_hand est partage).\n"
		"  --fire-no-chain <c> no-chain propre a la continuation post-injection\n"
		"                     (les --no-chain globaux y sont leves). Met en\n"
		"                     scene un contreur precis : interdire Crystal\n"
		"                     Wing force la voie Zalen+Junk Signal. Repetable.\n"
		"  --fire-open        n'injecter qu'aux fenetres OUVERTES (chaine\n"
		"                     vide) : la carte tiree DEMARRE une chaine au\n"
		"                     lieu d'etre chainee sur nos effets — la vraie\n"
		"                     menace adverse\n"
		"  --approach <f.yrp> approche d'une session passee (best_approach_*.yrp)\n"
		"  --rounds <n>       boucle INTERNE : n rounds se partagent --solve-ms,\n"
		"                     la meilleure ligne jointe de chaque round est\n"
		"                     reinjectee au suivant (defaut 1 = historique)\n"
		"                     servie au finisseur comme racine supplementaire\n"
		"                     (chemin complet + reculs). Repetable. Doit avoir\n"
		"                     ete produite sur le MEME duel de depart (et le\n"
		"                     meme --opp-hand).\n"
		"  --tt-mb <n>        table de transposition PARTAGEE entre workers\n"
		"                     (lazy SMP), en Mo par passe (defaut 64 ; 0 =\n"
		"                     tables privees)\n"
		"  --summon <spec>    contrainte : la n-ieme invocation (normale ou\n"
		"                     speciale, le decompte de Nibiru) doit etre une des\n"
		"                     cartes donnees. <spec> = n:carte[|carte...], carte =\n"
		"                     code ou fragment de nom (resolution unique exigee).\n"
		"                     Repetable. Ex : --summon \"5:Zalen|Crystal Wing\"\n"
		"  --guard <spec>     garde : a partir de la n-ieme invocation, a chaque\n"
		"                     fenetre de reponse ADVERSE (la ou Nibiru tombe), au\n"
		"                     moins une clause doit tenir. <spec> =\n"
		"                     n:clause[|clause...], clause = carte[@zone][+...],\n"
		"                     zones : main terrain cimetiere banni extra\n"
		"                     (defaut terrain). Ex : --guard \"5:Crystal Wing|\n"
		"                     Zalen@terrain+Junk Signal@main\"\n"
		"  --no-activate <c>  interdit d'activer cette carte depuis une zone\n"
		"                     (<c> = carte[@zone], defaut terrain — l'activation\n"
		"                     depuis la main, qui POSE la carte, reste permise).\n"
		"                     Repetable. Ex : --no-activate \"Assault Zone\"\n"
		"  --no-chain <c>     cette carte n'est jamais CHAINEE aux fenetres de\n"
		"                     reponse (en solitaire, toute chaine repond a nos\n"
		"                     propres actions : un garde qui annule nos cartes\n"
		"                     est une branche inutile). Les declencheurs forces\n"
		"                     et les commandes idle restent permis. Repetable.\n"
		"                     Ex : --no-chain Zalen --no-chain \"Crystal Wing\"\n"
		"  --guard-off <cond> eteint la garde quand la menace n'existe plus.\n"
		"                     Forme : mainadv<=N — la garde n'est plus exigee\n"
		"                     aux fenetres ou la main adverse compte au plus N\n"
		"                     cartes (un deck handrip vide la main de Nibiru).\n"
		"  --resolve <spec>   la ligne doit resoudre l'effet de cette carte au\n"
		"                     moins n fois avant le board (<spec> =\n"
		"                     carte[@zone][:n], defaut 1). @zone restreint la\n"
		"                     zone d'ACTIVATION (l'Omega qui rippe s'active du\n"
		"                     terrain — sans @terrain, son effet de cimetiere\n"
		"                     compterait aussi). Controle au BUT : un board\n"
		"                     conforme sans les resolutions n'est pas une\n"
		"                     solution. Repetable (max 4).\n"
		"                     Ex : --resolve \"Omega@terrain:2\"\n"
		"  --summon-min <s>   la ligne doit INVOQUER cette carte au moins n fois\n"
		"                     (<s> = carte[:n], defaut 1). Meme mecanique que\n"
		"                     --resolve (gate au but, gradient, biais), comptee\n"
		"                     sur les invocations. Partage la limite de 4.\n"
		"                     Ex : --summon-min \"Junk Meister\"\n"
		"  --profile          profil du chemin chaud (sondes rdtsc par phase,\n"
		"                     ligne « reste » comprise) ; l'instrument se paie,\n"
		"                     le chiffrer fait partie de la mesure\n"
		"  --verbose          trace chaque decision\n");
}

bool ParseArgs(int argc, char** argv, Options& o) {
	for(int i = 1; i < argc; ++i) {
		std::string a = argv[i];
		auto next = [&](const char* what) -> const char* {
			if(i + 1 >= argc) {
				std::printf("!! %s attend une valeur\n", what);
				return nullptr;
			}
			return argv[++i];
		};
		// DRAPEAUX BOOLEENS SIMPLES, sortis de la chaine `else if` ci-dessous.
		// MSVC plafonne l'imbrication a 128 blocs (C1061) et la chaine y etait :
		// tout nouveau drapeau sans valeur passe desormais par cette table, qui
		// ne coute rien et ne peut plus faire echouer la compilation.
		{
			static const struct { const char* name; bool Options::* member; }
			kBoolFlags[] = {
				{ "--assign",           &Options::assign },
				{ "--backward",         &Options::backward },
				{ "--elide-forced",     &Options::elide_forced },
				{ "--adapt-to-peak",    &Options::adapt_to_peak },
				{ "--operators",        &Options::operators },
				{ "--op-recipes",       &Options::op_recipes },
				{ "--quota-legacy",     &Options::quota_legacy },
				{ "--quota-h",          &Options::quota_h },
				{ "--archive-fin",      &Options::archive_fin },
				{ "--carry",            &Options::carry },
				{ "--resolve-legacy",   &Options::resolve_legacy },
				{ "--no-self-negate",   &Options::no_self_negate },
				{ "--mp1-only",         &Options::mp1_only },
			};
			bool matched = false;
			for(const auto& f : kBoolFlags)
				if(a == f.name) { o.*(f.member) = true; matched = true; break; }
			if(matched)
				continue;
			// DRAPEAUX NEGATIFS DES MECANISMES PROMUS EN DEFAUT (session 19).
			// Regle 2 du README : passe sur les DEUX etalons, un mecanisme
			// devient le defaut, et le drapeau devient negatif — il ne sert plus
			// qu'a rejouer l'A/B qui l'a fait promouvoir.
			{
				static const struct { const char* name; bool Options::* member; }
				kNoFlags[] = {
					{ "--no-adapt-to-peak", &Options::adapt_to_peak },
					{ "--no-serial",        &Options::serial },
				};
				for(const auto& f : kNoFlags)
					if(a == f.name) { o.*(f.member) = false; matched = true; break; }
				if(matched)
					continue;
			}
			if(a == "--no-hindsight") {
				o.hindsight = 0.0;
				continue;
			}
		}
		// DRAPEAUX A VALEUR ENTIERE NON SIGNEE, meme raison : la chaine `else if`
		// est a la limite du compilateur, et un drapeau de plus la faisait sauter
		// (C1061 mesure). Toute option a valeur ajoutee ensuite passe par ici.
		{
			static const struct { const char* name; uint64_t Options::* member; }
			kU64Flags[] = {
				{ "--max-rollouts", &Options::max_rollouts },
				{ "--max-nodes",    &Options::max_nodes },
				{ "--refine-after", &Options::refine_after },
				{ "--turns",        &Options::turns },
				{ "--rounds",       &Options::rounds },
			};
			bool matched = false;
			for(const auto& f : kU64Flags)
				if(a == f.name) {
					const char* v = next(f.name);
					if(!v)
						return false;
					o.*(f.member) = std::strtoull(v, nullptr, 10);
					matched = true;
					break;
				}
			if(matched)
				continue;
		}
		if(a == "--workdir") {
			const char* v = next("--workdir"); if(!v) return false;
			o.workdir = v;
		} else if(a == "--scriptdir") {
			const char* v = next("--scriptdir"); if(!v) return false;
			o.scriptdirs.emplace_back(v);
		} else if(a == "--player") {
			const char* v = next("--player"); if(!v) return false;
			o.target_player = std::atoi(v);
		} else if(a == "--arena-mb") {
			const char* v = next("--arena-mb"); if(!v) return false;
			o.arena_mb = static_cast<size_t>(std::atoi(v));
		} else if(a == "--no-arena") {
			o.no_arena = true;
		} else if(a == "--keep-gc") {
			o.stop_gc = false;
		} else if(a == "--growth") {
			o.growth = true;
		} else if(a == "--growth-max") {
			const char* v = next("--growth-max"); if(!v) return false;
			o.growth_max = static_cast<uint32_t>(std::atoi(v));
		} else if(a == "--growth-ms") {
			const char* v = next("--growth-ms"); if(!v) return false;
			o.growth_ms = std::atof(v);
		} else if(a == "--start") {
			const char* v = next("--start"); if(!v) return false;
			o.start_replay = v;
			o.solve = true;
		} else if(a == "--deck") {
			const char* v = next("--deck"); if(!v) return false;
			o.deck_file = v;
			o.solve = true;
		} else if(a == "--hand") {
			const char* v = next("--hand"); if(!v) return false;
			o.hand_spec = v;
		} else if(a == "--hint") {
			const char* v = next("--hint"); if(!v) return false;
			o.hint_specs.emplace_back(v);
		} else if(a == "--approach") {
			const char* v = next("--approach"); if(!v) return false;
			o.approach_files.emplace_back(v);
		} else if(a == "--opp-hand") {
			const char* v = next("--opp-hand"); if(!v) return false;
			o.opp_hand_specs.emplace_back(v);
		} else if(a == "--board-add") {
			const char* v = next("--board-add"); if(!v) return false;
			o.board_add_specs.emplace_back(v);
		} else if(a == "--board-remove") {
			const char* v = next("--board-remove"); if(!v) return false;
			o.board_remove_specs.emplace_back(v);
		} else if(a == "--material") {
			const char* v = next("--material"); if(!v) return false;
			o.material_specs.emplace_back(v);
		} else if(a == "--outdir") {
			const char* v = next("--outdir"); if(!v) return false;
			o.outdir = v;
		} else if(a == "--solve") {
			o.solve = true;
		} else if(a == "--solve-ms") {
			const char* v = next("--solve-ms"); if(!v) return false;
			o.solve_ms = std::atof(v);
		} else if(a == "--threads") {
			const char* v = next("--threads"); if(!v) return false;
			o.threads = static_cast<unsigned>(std::atoi(v));
		} else if(a == "--width") {
			o.width = true;
		} else if(a == "--novelty") {
			const char* v = next("--novelty"); if(!v) return false;
			o.novelty = std::atoi(v);
		} else if(a == "--no-novelty") {
			o.novelty = 0;
		} else if(a == "--no-nrpa") {
			o.nrpa = false;
		} else if(a == "--seed") {
			const char* v = next("--seed"); if(!v) return false;
			o.seed = std::strtoull(v, nullptr, 10);
		} else if(a == "--nrpa-bias") {
			const char* v = next("--nrpa-bias"); if(!v) return false;
			o.nrpa_bias = std::atof(v);
		} else if(a == "--nrpa-keep") {
			const char* v = next("--nrpa-keep"); if(!v) return false;
			o.nrpa_keep = std::atof(v);
		} else if(a == "--nrpa-alpha") {
			const char* v = next("--nrpa-alpha"); if(!v) return false;
			o.nrpa_alpha = std::atof(v);
		} else if(a == "--nrpa-iters") {
			const char* v = next("--nrpa-iters"); if(!v) return false;
			o.nrpa_iters = static_cast<uint32_t>(std::atoi(v));
		} else if(a == "--finisher") {
			const char* v = next("--finisher"); if(!v) return false;
			o.finisher = v;
			if(o.finisher != "levin" && o.finisher != "mono" &&
			   o.finisher != "ab") {
				std::printf("!! --finisher attend levin, mono ou ab\n");
				return false;
			}
		} else if(a == "--archive-k") {
			const char* v = next("--archive-k"); if(!v) return false;
			o.archive_k = static_cast<size_t>(std::atoi(v));
		} else if(a == "--finisher-min") {
			const char* v = next("--finisher-min"); if(!v) return false;
			o.finisher_min = std::atof(v);
		} else if(a == "--levin-h") {
			const char* v = next("--levin-h"); if(!v) return false;
			o.levin_h = std::atof(v);
		} else if(a == "--resolve-weight") {
			const char* v = next("--resolve-weight"); if(!v) return false;
			o.resolve_weight = std::atof(v);
		} else if(a == "--optimize") {
			o.optimize = true;
		} else if(a == "--fire") {
			const char* v = next("--fire"); if(!v) return false;
			o.fire_spec = v;
		} else if(a == "--fire-spare") {
			const char* v = next("--fire-spare"); if(!v) return false;
			o.fire_spare_specs.emplace_back(v);
		} else if(a == "--fire-ms") {
			const char* v = next("--fire-ms"); if(!v) return false;
			o.fire_ms = std::atof(v);
		} else if(a == "--fire-bake") {
			o.fire_bake = true;
		} else if(a == "--fire-no-chain") {
			const char* v = next("--fire-no-chain"); if(!v) return false;
			o.fire_no_chain_specs.emplace_back(v);
		} else if(a == "--fire-open") {
			o.fire_open = true;
		} else if(a == "--burn-slack") {
			const char* v = next("--burn-slack"); if(!v) return false;
			o.burn_slack = static_cast<uint32_t>(std::atoi(v));
		} else if(a == "--burn-limit") {
			const char* v = next("--burn-limit"); if(!v) return false;
			o.burn_limit = static_cast<uint32_t>(std::atoi(v));
		} else if(a == "--no-burn-share") {
			o.burn_share = false;
		} else if(a == "--prior") {
			const char* v = next("--prior"); if(!v) return false;
			o.prior_files.emplace_back(v);
		} else if(a == "--prior-weight") {
			const char* v = next("--prior-weight"); if(!v) return false;
			o.prior_weight = std::atof(v);
		} else if(a == "--adapt") {
			const char* v = next("--adapt"); if(!v) return false;
			o.adapt_files.emplace_back(v);
		} else if(a == "--adapt-passes") {
			const char* v = next("--adapt-passes"); if(!v) return false;
			o.adapt_passes = static_cast<uint32_t>(std::atoi(v));
		} else if(a == "--options") {
			const char* v = next("--options"); if(!v) return false;
			o.options_n = static_cast<uint32_t>(std::atoi(v));
		} else if(a == "--options-support") {
			const char* v = next("--options-support"); if(!v) return false;
			o.options_support = static_cast<uint32_t>(std::atoi(v));
		} else if(a == "--options-len") {
			const char* v = next("--options-len"); if(!v) return false;
			o.options_len = static_cast<uint32_t>(std::atoi(v));
		} else if(a == "--options-window") {
			const char* v = next("--options-window"); if(!v) return false;
			o.options_window = static_cast<uint32_t>(std::atoi(v));
		} else if(a == "--options-ctx") {
			const char* v = next("--options-ctx"); if(!v) return false;
			o.options_ctx = std::atoi(v);
		} else if(a == "--options-online") {
			const char* v = next("--options-online"); if(!v) return false;
			o.options_online = static_cast<uint32_t>(std::atoi(v));
		} else if(a == "--options-pool") {
			const char* v = next("--options-pool"); if(!v) return false;
			o.options_pool = static_cast<uint32_t>(std::atoi(v));
		} else if(a == "--options-per-worker") {
			const char* v = next("--options-per-worker"); if(!v) return false;
			o.options_per_worker = static_cast<uint32_t>(std::atoi(v));
		} else if(a == "--merged-pop") {
			o.merged_pop = true;
		} else if(a == "--no-merged-pop") {
			o.merged_pop = false;
		} else if(a == "--finisher-post-goal") {
			o.finisher_post_goal = true;
		} else if(a == "--finisher-options") {
			o.finisher_options = true;
		} else if(a == "--max-decisions") {
			const char* v = next("--max-decisions"); if(!v) return false;
			o.max_decisions = static_cast<uint32_t>(std::atoi(v));
		} else if(a == "--reroot") {
			o.levin_reroot = true;
		} else if(a == "--reroot-h") {
			const char* v = next("--reroot-h"); if(!v) return false;
			o.reroot_h = std::atof(v);
		} else if(a == "--dive-full") {
			o.dive_full = true;
		} else if(a == "--no-dive-full") {
			o.dive_full = false;
		} else if(a == "--lifo-ties") {
			o.lifo_ties = true;
		} else if(a == "--no-plan") {
			o.no_plan = true;
		} else if(a == "--no-ref") {
			o.no_ref = true;
			o.no_plan = true;
		} else if(a == "--target") {
			const char* v = next("--target"); if(!v) return false;
			o.target_specs.emplace_back(v);
		} else if(a == "--nrpa-temp") {
			const char* v = next("--nrpa-temp"); if(!v) return false;
			o.nrpa_temp = std::atof(v);
		} else if(a == "--max-subsets") {
			const char* v = next("--max-subsets"); if(!v) return false;
			const int n = std::atoi(v);
			if(n < 1 || n > 4096) {
				std::printf("!! --max-subsets attend 1..4096 (recu %s)\n", v);
				return false;
			}
			o.max_subsets = static_cast<uint32_t>(n);
		} else if(a == "--recipes") {
			const char* v = next("--recipes"); if(!v) return false;
			o.recipes = std::atof(v);
			if(o.recipes < 0) {
				std::printf("!! --recipes attend un poids >= 0 (0 = alimente et "
							"mesure sans entrer dans le cout)\n");
				return false;
			}
		} else if(a == "--landmarks") {
			const char* v = next("--landmarks"); if(!v) return false;
			o.landmark_files.emplace_back(v);
		} else if(a == "--landmark-w") {
			const char* v = next("--landmark-w"); if(!v) return false;
			o.landmark_weight = std::atof(v);
			if(o.landmark_weight < 0) {
				std::printf("!! --landmark-w attend un poids >= 0\n");
				return false;
			}
		} else if(a == "--landmark-h") {
			const char* v = next("--landmark-h"); if(!v) return false;
			o.landmark_h = std::atof(v);
			if(o.landmark_h < 0) {
				std::printf("!! --landmark-h attend un poids >= 0\n");
				return false;
			}
		} else if(a == "--hindsight") {
			const char* v = next("--hindsight"); if(!v) return false;
			o.hindsight = std::atof(v);
			if(o.hindsight < 0) {
				std::printf("!! --hindsight attend une fraction d'alpha >= 0\n");
				return false;
			}
		} else if(a == "--hindsight-k") {
			const char* v = next("--hindsight-k"); if(!v) return false;
			const long k = std::atol(v);
			if(k <= 0) {
				std::printf("!! --hindsight-k attend un entier > 0\n");
				return false;
			}
			o.hindsight_k = static_cast<size_t>(k);
		} else if(a == "--assign-bias") {
			const char* v = next("--assign-bias"); if(!v) return false;
			o.assign_bias = std::atof(v);
			if(o.assign_bias < 0) {
				std::printf("!! --assign-bias attend un poids >= 0\n");
				return false;
			}
		} else if(a == "--reenter") {
			// Retour au barreau (s21) : probabilite de re-entree par cellule.
			const char* v = next("--reenter"); if(!v) return false;
			o.reenter = std::atof(v);
			if(o.reenter < 0.0 || o.reenter > 1.0) {
				std::printf("!! --reenter attend une probabilite dans [0, 1]\n");
				return false;
			}
		} else if(a == "--phase-w") {
			// Poids SOUSTRAIT au logit d'un changement de phase. Ce n'est pas
			// un elagage : le choix reste tirable (regle 2), sa masse baisse.
			const char* v = next("--phase-w"); if(!v) return false;
			o.phase_w = std::atof(v);
		} else if(a == "--op-bias") {
			const char* v = next("--op-bias"); if(!v) return false;
			o.op_bias = std::atof(v);
			if(o.op_bias < 0) {
				std::printf("!! --op-bias attend un poids >= 0\n");
				return false;
			}
		} else if(a == "--watch") {
			const char* v = next("--watch"); if(!v) return false;
			o.watch_specs.emplace_back(v);
		} else if(a == "--probe-repeat") {
			o.probe_repeat = true;
		} else if(false) {
		} else if(a == "--no-seed-recipes") {
			o.seed_recipes = false;
		} else if(a == "--no-seed-quant") {
			o.seed_cardinal = false;
		} else if(a == "--derive-summon-min") {
			o.derive_summon_min = true;
		} else if(a == "--hint-bias") {
			const char* v = next("--hint-bias"); if(!v) return false;
			o.hint_bias = std::atof(v);
		} else if(a == "--nrpa-level") {
			const char* v = next("--nrpa-level"); if(!v) return false;
			o.nrpa_level = std::atoi(v);
			if(o.nrpa_level < 1 || o.nrpa_level > 4) {
				std::printf("!! --nrpa-level attend 1..4 (recu %s) ; 2 coute "
							"~576 tirages par appel, 3 en coute ~13 824\n", v);
				return false;
			}
		} else if(a == "--ctx-shrink") {
			const char* v = next("--ctx-shrink"); if(!v) return false;
			o.ctx_shrink = std::atof(v);
		} else if(a == "--qhat") {
			const char* v = next("--qhat"); if(!v) return false;
			o.qhat_depth = static_cast<uint32_t>(std::atoi(v));
		} else if(a == "--qhat-window") {
			const char* v = next("--qhat-window"); if(!v) return false;
			o.qhat_window = static_cast<uint32_t>(std::atoi(v));
		} else if(a == "--qhat-rho") {
			const char* v = next("--qhat-rho"); if(!v) return false;
			o.qhat_rho = static_cast<uint32_t>(std::atoi(v));
		} else if(a == "--qhat-nodes") {
			const char* v = next("--qhat-nodes"); if(!v) return false;
			o.qhat_nodes = static_cast<size_t>(std::atoll(v));
		} else if(a == "--no-qhat-probe") {
			o.qhat_probe = false;
		} else if(false) {
		} else if(false) {
		} else if(a == "--canonical-zones") {
			o.canonical_zones = true;
		} else if(false) {
		} else if(a == "--target-subset") {
			o.target_subset = true;
		} else if(a == "--target-exact") {
			o.target_exact = true;
		} else if(a == "--ctx-max") {
			const char* v = next("--ctx-max"); if(!v) return false;
			o.ctx_max = static_cast<size_t>(std::atoll(v));
		} else if(a == "--tt-mb") {
			const char* v = next("--tt-mb"); if(!v) return false;
			o.tt_mb = static_cast<size_t>(std::atoi(v));
		} else if(a == "--summon") {
			const char* v = next("--summon"); if(!v) return false;
			o.summon_specs.emplace_back(v);
		} else if(a == "--guard") {
			const char* v = next("--guard"); if(!v) return false;
			o.guard_specs.emplace_back(v);
		} else if(a == "--no-activate") {
			const char* v = next("--no-activate"); if(!v) return false;
			o.no_activate_specs.emplace_back(v);
		} else if(a == "--no-chain") {
			const char* v = next("--no-chain"); if(!v) return false;
			o.no_chain_specs.emplace_back(v);
		} else if(a == "--guard-off") {
			const char* v = next("--guard-off"); if(!v) return false;
			o.guard_off_spec = v;
		} else if(a == "--resolve") {
			const char* v = next("--resolve"); if(!v) return false;
			o.resolve_specs.emplace_back(v);
		} else if(a == "--summon-min") {
			const char* v = next("--summon-min"); if(!v) return false;
			o.summon_min_specs.emplace_back(v);
		} else if(a == "--profile") {
			o.profile = true;
		} else if(a == "--verbose" || a == "-v") {
			o.verbose = true;
		} else if(a == "--help" || a == "-h") {
			return false;
		} else if(!a.empty() && a[0] == '-') {
			std::printf("!! option inconnue : %s\n", a.c_str());
			return false;
		} else if(o.replay.empty()) {
			o.replay = a;
		} else {
			std::printf("!! argument en trop : %s\n", a.c_str());
			return false;
		}
	}
	// `--mcps` (conditionnement du LOGIT par le chemin) a vecu ici. SUPPRIME
	// (audit 18) : REFUTE DEUX FOIS — 0 comparaison gagnee sur 4 contre
	// `--ctx-shrink` seul, effondrement total a k = 6, meme palier d'accord
	// (9.21 (k)). Et la refutation portait une lecon de fond : il greffait le
	// conditionnement de MCPS sur un LOGIT NRPA, alors que le papier conditionne
	// une MOYENNE DE RECOMPENSE — c'est `--qhat` qui implemente le mecanisme.
	return !o.replay.empty();
}

// Une carte se donne par code ou par fragment de nom ; un fragment qui ne
// designe pas exactement une carte est une erreur qui liste les candidats —
// deviner a la place de l'utilisateur serait pire que refuser.
bool ResolveCard(const std::string& item, const CardDB& db, const char* flag,
				 uint32_t& out) {
	char* end = nullptr;
	unsigned long code = std::strtoul(item.c_str(), &end, 10);
	if(end && *end == '\0' && code > 1000) {
		out = db.Canonical(static_cast<uint32_t>(code));
		return true;
	}
	auto matches = db.FindByName(item);
	if(matches.size() == 1) {
		out = matches[0].first;
		return true;
	}
	if(matches.empty()) {
		std::printf("!! %s : aucune carte ne contient \"%s\"\n", flag, item.c_str());
	} else {
		std::printf("!! %s : \"%s\" est ambigu (%zu cartes) :\n", flag,
					item.c_str(), matches.size());
		for(size_t i = 0; i < matches.size() && i < 8; ++i)
			std::printf("     %9u  %s\n", matches[i].first,
						matches[i].second.c_str());
		if(matches.size() > 8)
			std::printf("     ...\n");
	}
	return false;
}

std::string Trimmed(std::string s) {
	while(!s.empty() && s.front() == ' ') s.erase(s.begin());
	while(!s.empty() && s.back() == ' ') s.pop_back();
	return s;
}

std::vector<std::string> SplitOn(const std::string& s, char sep) {
	std::vector<std::string> out;
	size_t pos = 0;
	while(pos <= s.size()) {
		size_t at = s.find(sep, pos);
		out.push_back(Trimmed(at == std::string::npos
			? s.substr(pos) : s.substr(pos, at - pos)));
		pos = (at == std::string::npos) ? s.size() + 1 : at + 1;
	}
	return out;
}

bool ZoneMaskOf(const std::string& z, uint32_t& mask) {
	if(z == "main")           mask = LOCATION_HAND;
	else if(z == "terrain")   mask = LOCATION_MZONE | LOCATION_SZONE;
	else if(z == "cimetiere") mask = LOCATION_GRAVE;
	else if(z == "banni")     mask = LOCATION_REMOVED;
	else if(z == "extra")     mask = LOCATION_EXTRA;
	else return false;
	return true;
}

// Nom lisible d'un masque de zones (inverse de ZoneMaskOf, pour l'affichage).
std::string ZoneMaskName(uint32_t mask) {
	std::string s;
	auto add = [&](uint32_t m, const char* n) {
		if(mask & m) {
			if(!s.empty())
				s += "+";
			s += n;
		}
	};
	add(LOCATION_HAND, "main");
	add(LOCATION_MZONE | LOCATION_SZONE, "terrain");
	add(LOCATION_GRAVE, "cimetiere");
	add(LOCATION_REMOVED, "banni");
	add(LOCATION_EXTRA, "extra");
	return s;
}

// "carte[@zone]" -> (code canonique, masque). Zone par defaut : terrain.
bool ResolveCardZone(const std::string& item, const CardDB& db, const char* flag,
					 uint32_t& code, uint32_t& zones) {
	size_t at = item.rfind('@');
	std::string card = (at == std::string::npos) ? item : item.substr(0, at);
	std::string zone = (at == std::string::npos) ? "terrain" : item.substr(at + 1);
	if(!ZoneMaskOf(Trimmed(zone), zones)) {
		std::printf("!! %s : zone inconnue \"%s\" (main terrain cimetiere banni "
					"extra)\n", flag, zone.c_str());
		return false;
	}
	return ResolveCard(Trimmed(card), db, flag, code);
}

// Resout toutes les contraintes CLI. Toute erreur arrete AVANT la recherche.
bool ResolveConstraints(const Options& opt, const CardDB& db,
						LineConstraints& out) {
	for(const std::string& spec : opt.summon_specs) {
		size_t colon = spec.find(':');
		int n = (colon == std::string::npos)
			? 0 : std::atoi(spec.substr(0, colon).c_str());
		if(n <= 0) {
			std::printf("!! --summon \"%s\" : forme attendue n:carte[|carte...], "
						"n >= 1\n", spec.c_str());
			return false;
		}
		std::vector<uint32_t> allowed;
		for(const std::string& item : SplitOn(spec.substr(colon + 1), '|')) {
			if(item.empty())
				continue;
			uint32_t code = 0;
			if(!ResolveCard(item, db, "--summon", code))
				return false;
			allowed.push_back(code);
		}
		if(allowed.empty()) {
			std::printf("!! --summon \"%s\" : aucune carte donnee\n", spec.c_str());
			return false;
		}
		auto& slot = out.summons[static_cast<uint32_t>(n)];
		slot.insert(slot.end(), allowed.begin(), allowed.end());
	}

	if(opt.guard_specs.size() > 1) {
		std::printf("!! --guard : une seule garde a la fois\n");
		return false;
	}
	for(const std::string& spec : opt.guard_specs) {
		size_t colon = spec.find(':');
		int n = (colon == std::string::npos)
			? 0 : std::atoi(spec.substr(0, colon).c_str());
		if(n <= 0) {
			std::printf("!! --guard \"%s\" : forme attendue n:clause[|clause...]\n",
						spec.c_str());
			return false;
		}
		out.guard_after = static_cast<uint32_t>(n);
		for(const std::string& clause_s : SplitOn(spec.substr(colon + 1), '|')) {
			if(clause_s.empty())
				continue;
			GuardClause clause;
			for(const std::string& atom_s : SplitOn(clause_s, '+')) {
				if(atom_s.empty())
					continue;
				GuardAtom a;
				if(!ResolveCardZone(atom_s, db, "--guard", a.code, a.zones))
					return false;
				clause.push_back(a);
			}
			if(!clause.empty())
				out.guard.push_back(std::move(clause));
		}
		if(out.guard.empty()) {
			std::printf("!! --guard \"%s\" : aucune clause\n", spec.c_str());
			return false;
		}
	}

	for(const std::string& spec : opt.no_activate_specs) {
		uint32_t code = 0, zones = 0;
		if(!ResolveCardZone(spec, db, "--no-activate", code, zones))
			return false;
		out.no_activate[code] |= zones;
	}

	for(const std::string& spec : opt.no_chain_specs) {
		uint32_t code = 0;
		if(!ResolveCard(spec, db, "--no-chain", code))
			return false;
		if(std::find(out.no_chain.begin(), out.no_chain.end(), code) ==
		   out.no_chain.end())
			out.no_chain.push_back(code);
	}

	for(const std::string& spec : opt.resolve_specs) {
		if(out.resolve_min.size() >= 4) {
			std::printf("!! --resolve : au plus 4 cartes surveillees\n");
			return false;
		}
		// "carte[:n]" — le n est le suffixe apres le DERNIER ':' s'il est
		// numerique ; certains noms contiennent un ':' (Number 39: Utopia).
		std::string card = spec;
		uint32_t n = 1;
		size_t colon = spec.rfind(':');
		if(colon != std::string::npos) {
			const std::string tail = Trimmed(spec.substr(colon + 1));
			char* end = nullptr;
			unsigned long v = std::strtoul(tail.c_str(), &end, 10);
			if(end && *end == '\0' && v > 0 && v < 0xffff) {
				n = static_cast<uint32_t>(v);
				card = spec.substr(0, colon);
			}
		}
		// Zone d'activation optionnelle : "carte[@zone]". Defaut : toutes les
		// zones (comportement d'avant) — le filtre se demande explicitement.
		ResolveReq req;
		req.min_count = n;
		size_t at = card.rfind('@');
		if(at != std::string::npos) {
			if(!ZoneMaskOf(Trimmed(card.substr(at + 1)), req.zones)) {
				std::printf("!! --resolve : zone inconnue \"%s\" (main terrain "
							"cimetiere banni extra)\n",
							card.substr(at + 1).c_str());
				return false;
			}
			card = card.substr(0, at);
		}
		if(!ResolveCard(Trimmed(card), db, "--resolve", req.code))
			return false;
		out.resolve_min.push_back(req);
	}

	for(const std::string& spec : opt.summon_min_specs) {
		if(out.resolve_min.size() >= 4) {
			std::printf("!! --resolve/--summon-min : au plus 4 cartes "
						"surveillees\n");
			return false;
		}
		std::string card = spec;
		uint32_t n = 1;
		size_t colon = spec.rfind(':');
		if(colon != std::string::npos) {
			const std::string tail = Trimmed(spec.substr(colon + 1));
			char* end = nullptr;
			unsigned long v = std::strtoul(tail.c_str(), &end, 10);
			if(end && *end == '\0' && v > 0 && v < 0xffff) {
				n = static_cast<uint32_t>(v);
				card = spec.substr(0, colon);
			}
		}
		ResolveReq req;
		req.min_count = n;
		req.on_summon = true;
		if(!ResolveCard(Trimmed(card), db, "--summon-min", req.code))
			return false;
		out.resolve_min.push_back(req);
	}

	for(const std::string& spec : opt.hint_specs) {
		uint32_t code = 0;
		if(!ResolveCard(Trimmed(spec), db, "--hint", code))
			return false;
		out.hints.push_back(code);
	}

	for(const std::string& spec : opt.opp_hand_specs) {
		for(const std::string& item : SplitOn(spec, '|')) {
			if(item.empty())
				continue;
			uint32_t code = 0;
			if(!ResolveCard(Trimmed(item), db, "--opp-hand", code))
				return false;
			out.opp_hand.push_back(code);
		}
	}

	// --board-add et --target partagent la grammaire "carte[@ATK|DEF]" : le
	// premier AJOUTE a la capture de la reference, le second CONSTRUIT la cible
	// de zero. Une seule analyse, pour qu'ils ne divergent jamais.
	auto parse_board_card = [&](const std::string& spec, const char* flag) -> bool {
		size_t at = spec.rfind('@');
		std::string card = (at == std::string::npos) ? spec : spec.substr(0, at);
		std::string pos = (at == std::string::npos)
			? "ATK" : Trimmed(spec.substr(at + 1));
		uint32_t position = 0;
		if(pos == "ATK")      position = POS_FACEUP_ATTACK;
		else if(pos == "DEF") position = POS_FACEUP_DEFENSE;
		else {
			std::printf("!! %s : position inconnue \"%s\" (ATK ou DEF)\n",
						flag, pos.c_str());
			return false;
		}
		uint32_t code = 0;
		if(!ResolveCard(Trimmed(card), db, flag, code))
			return false;
		out.board_add.emplace_back(code, position);
		return true;
	};
	for(const std::string& spec : opt.board_add_specs)
		if(!parse_board_card(spec, "--board-add"))
			return false;
	if(!opt.target_specs.empty()) {
		out.target_scratch = true;
		for(const std::string& spec : opt.target_specs)
			if(!parse_board_card(spec, "--target"))
				return false;
	}
	for(const std::string& spec : opt.board_remove_specs) {
		uint32_t code = 0;
		if(!ResolveCard(Trimmed(spec), db, "--board-remove", code))
			return false;
		out.board_remove.push_back(code);
	}

	for(const std::string& spec : opt.material_specs) {
		size_t colon = spec.rfind(':');
		if(colon == std::string::npos) {
			std::printf("!! --material \"%s\" : forme attendue carte:attr[,attr]\n",
						spec.c_str());
			return false;
		}
		uint32_t mask = 0;
		bool attrs_ok = true;
		for(const std::string& a : SplitOn(spec.substr(colon + 1), ',')) {
			if(a.empty())
				continue;
			if(a == "lumiere" || a == "light")        mask |= ATTRIBUTE_LIGHT;
			else if(a == "tenebres" || a == "dark")   mask |= ATTRIBUTE_DARK;
			else if(a == "terre")                     mask |= ATTRIBUTE_EARTH;
			else if(a == "eau")                       mask |= ATTRIBUTE_WATER;
			else if(a == "feu")                       mask |= ATTRIBUTE_FIRE;
			else if(a == "vent")                      mask |= ATTRIBUTE_WIND;
			else if(a == "divin")                     mask |= ATTRIBUTE_DIVINE;
			else { attrs_ok = false; break; }
		}
		if(!attrs_ok || !mask) {
			std::printf("!! --material \"%s\" : attributs attendus apres ':' "
						"(lumiere tenebres terre eau feu vent divin)\n",
						spec.c_str());
			return false;
		}
		uint32_t code = 0;
		if(!ResolveCard(Trimmed(spec.substr(0, colon)), db, "--material", code))
			return false;
		out.material_req.emplace_back(code, mask);
	}

	if(!opt.guard_off_spec.empty()) {
		const std::string s = Trimmed(opt.guard_off_spec);
		const std::string prefix = "mainadv<=";
		if(s.compare(0, prefix.size(), prefix) != 0) {
			std::printf("!! --guard-off \"%s\" : forme attendue mainadv<=N\n",
						s.c_str());
			return false;
		}
		out.guard_opp_hand_release = std::atoi(s.c_str() + prefix.size());
		if(out.guard.empty()) {
			std::printf("!! --guard-off sans --guard : rien a eteindre\n");
			return false;
		}
	}
	return true;
}

const char* PhaseName(uint32_t p) {
	switch(p) {
	case PHASE_DRAW:    return "DRAW";
	case PHASE_STANDBY: return "STANDBY";
	case PHASE_MAIN1:   return "MAIN1";
	case PHASE_BATTLE:  return "BATTLE";
	case PHASE_MAIN2:   return "MAIN2";
	case PHASE_END:     return "END";
	default:            return "?";
	}
}

const char* PosName(uint32_t p) {
	switch(p) {
	case POS_FACEUP_ATTACK:    return "ATK";
	case POS_FACEDOWN_ATTACK:  return "FD-ATK";
	case POS_FACEUP_DEFENSE:   return "DEF";
	case POS_FACEDOWN_DEFENSE: return "FD-DEF";
	case POS_FACEUP:           return "FACEUP";
	case POS_FACEDOWN:         return "FACEDOWN";
	default:                   return "?";
	}
}

constexpr uint32_t kBoardFlags = QUERY_CODE | QUERY_ALIAS | QUERY_POSITION |
								 QUERY_TYPE | QUERY_LEVEL | QUERY_ATTACK |
								 QUERY_DEFENSE | QUERY_OVERLAY_CARD |
								 QUERY_COUNTERS | QUERY_LINK;

// Etat des zones d'un joueur au sens du critere d'equivalence retenu :
// terrain avec positions et materiaux, comptes pour les zones cachees.
//
// Le contenu de la main, du cimetiere et de la zone bannie n'entre PAS dans
// l'equivalence, mais il dit quelles cartes la ligne a consommees — la seule
// facon de savoir si un autre deck peut esperer refaire le meme board.
struct Board {
	std::vector<QueriedCard> mzone, szone;
	std::vector<QueriedCard> hand_cards, grave_cards, removed_cards;
	uint32_t hand{}, deck{}, extra{}, grave{}, removed{};
};

Board Snapshot(Duel& duel, uint8_t con) {
	Board b;
	b.mzone = duel.Query(con, LOCATION_MZONE, kBoardFlags);
	b.szone = duel.Query(con, LOCATION_SZONE, kBoardFlags);
	b.hand_cards = duel.Query(con, LOCATION_HAND, kBoardFlags);
	b.grave_cards = duel.Query(con, LOCATION_GRAVE, kBoardFlags);
	b.removed_cards = duel.Query(con, LOCATION_REMOVED, kBoardFlags);
	b.hand = duel.Count(con, LOCATION_HAND);
	b.deck = duel.Count(con, LOCATION_DECK);
	b.extra = duel.Count(con, LOCATION_EXTRA);
	b.grave = duel.Count(con, LOCATION_GRAVE);
	b.removed = duel.Count(con, LOCATION_REMOVED);
	return b;
}

// Multiensemble des cartes PHYSIQUES d'une zone.
//
// Deliberement `c.code` et non `c.Code()` : le second passe par get_code(), qui
// rend le nom EFFECTIF — un monstre dont un effet change le nom y apparaitrait
// comme la carte qu'il imite. Cela convient pour comparer deux terrains, pas
// pour compter ce qu'un deck doit contenir. Seul l'alias d'illustration est
// resolu, parce que deux illustrations sont bien le meme exemplaire.
std::map<uint32_t, uint32_t> CodeCounts(const std::vector<QueriedCard>& zone,
										const CardDB& db) {
	std::map<uint32_t, uint32_t> out;
	for(const auto& c : zone)
		if(c.present)
			++out[db.Canonical(c.code)];
	return out;
}

void PrintBoard(const Board& b, const CardDB& db) {
	auto dump = [&](const char* label, const std::vector<QueriedCard>& zone) {
		for(size_t i = 0; i < zone.size(); ++i) {
			const auto& c = zone[i];
			if(!c.present)
				continue;
			std::string extra;
			if(!c.overlay.empty())
				extra += " +" + std::to_string(c.overlay.size()) + " mat";
			if(!c.counters.empty())
				extra += " +compteurs";
			std::printf("      %s[%zu] %9u  %-36.36s %-8s%s\n", label, i, c.Code(),
						db.Name(c.Code()).c_str(), PosName(c.position), extra.c_str());
		}
	};
	dump("MZONE", b.mzone);
	dump("SZONE", b.szone);
	std::printf("      HAND=%u  DECK=%u  EXTRA=%u  GRAVE=%u  REMOVED=%u\n",
				b.hand, b.deck, b.extra, b.grave, b.removed);
}

size_t CountPresent(const std::vector<QueriedCard>& v) {
	return static_cast<size_t>(
		std::count_if(v.begin(), v.end(), [](const QueriedCard& c) { return c.present; }));
}

// Empreinte complete d'un etat visible, assez fine pour detecter une
// restauration infidele. Ce n'est pas encore le digest de transposition du
// solveur : il devra aussi couvrir l'etat du processeur.
uint64_t Fingerprint(Duel& duel) {
	uint64_t h = 1469598103934665603ull;
	auto mix = [&h](uint64_t v) { h ^= v; h *= 1099511628211ull; };
	constexpr uint32_t flags = kBoardFlags | QUERY_STATUS;
	for(uint8_t con = 0; con < 2; ++con) {
		for(uint32_t loc : { LOCATION_MZONE, LOCATION_SZONE, LOCATION_HAND,
							 LOCATION_GRAVE, LOCATION_REMOVED, LOCATION_EXTRA,
							 LOCATION_DECK }) {
			mix(loc * 0x9e3779b97f4a7c15ull + con);
			for(const auto& c : duel.Query(con, loc, flags)) {
				if(!c.present) { mix(0); continue; }
				mix(c.code);
				mix(c.position);
				mix((uint64_t(uint32_t(c.attack)) << 32) ^ uint32_t(c.defense));
				mix(c.status);
				mix((uint64_t(c.link) << 32) ^ c.link_marker);
				for(uint32_t o : c.overlay) mix(o * 31ull);
				for(uint32_t k : c.counters) mix(k * 37ull);
			}
		}
	}
	return h;
}

struct Stat {
	int n = 0;
	long double raw_sum = 0, dedup_sum = 0;
	long double raw_max = 0, dedup_max = 0;
	int undecoded = 0;
	int forced = 0;   // une seule reponse legale : candidat a l'elision
	int binary = 0;
};

struct LineResult {
	size_t responses_used = 0;
	size_t retries = 0;
	int turns = 0;
	long long summon = 0, spsummon = 0, flipsummon = 0, chaining = 0;
	std::map<uint8_t, Stat> stats;
	long double log_raw = 0, log_dedup = 0;
	bool have_target = false;
	// La cible a ete prise a la FIN de l'enregistrement et non a un changement
	// de tour : la ligne ne franchit pas la fin du tour (hand test arrete une
	// fois le combo pose). A signaler, car les controles qui supposent un tour
	// complet (coupure de tour, pic de brulees) se lisent differemment.
	bool target_at_is_end = false;
	Board target_self, target_oppo;
	size_t target_at = 0;
	// Position de depart, capturee au tout premier point de decision : c'est
	// elle qui dit si un deck a seulement de quoi commencer.
	bool have_start = false;
	Board start_self;
	// Sequence des invocations (normales + speciales, l'ordre de Nibiru), et
	// combien avaient eu lieu quand le board cible a ete capture. C'est contre
	// elle que les contraintes --summon jugent la reference.
	std::vector<uint32_t> summon_codes;
	size_t summons_at_target = 0;
	// Verdict de la reference face a --guard et --no-activate, evalue au fil
	// du rejeu instrumente (les fenetres adverses et les reponses enregistrees
	// ne se reconstituent pas apres coup).
	size_t guard_checks = 0, guard_violations = 0;
	size_t first_guard_violation_summon = 0;
	uint32_t first_guard_violation_opp_hand = 0;
	size_t forbidden_activations = 0;
	// Resolutions des cartes surveillees (--resolve), alignees sur
	// cons->resolve_min, comptees jusqu'au board.
	std::vector<size_t> resolve_counts;
	// Brulees (cimetiere + bannies) : pic en cours de ligne et compte au
	// board. L'ecart entre les deux MESURE la marge de recuperation — c'est
	// lui qui calibre --burn-slack (les brulees ne sont pas monotones).
	uint32_t burned_max = 0;
	uint32_t burned_at_target = 0;
	uint64_t fingerprint_at_target = 0;
	uint64_t fingerprint_final = 0;
	double ms = 0;
	// Pages salies entre deux decisions consecutives. C'est LA granularite qui
	// compte : le solveur branche a chaque decision, pas a chaque action, donc
	// c'est a ce rythme qu'il paiera un instantane.
	std::vector<size_t> dirty_per_decision;
	double ms_write_watch = 0;   // cout cumule des appels GetWriteWatch
	size_t write_watch_calls = 0;
	// Activations relevees dans la ligne (--operators). Vide autrement : le
	// harnais ne doit rien couter au rejeu ordinaire.
	std::vector<ObservedActivation> activations;
};

// Deroule les reponses enregistrees. `instrument` active la collecte complete ;
// une seconde passe de verification n'en a pas besoin. `cons` (facultatif)
// fait juger la reference contre --guard et --no-activate pendant le rejeu.
LineResult RunLine(Duel& duel, const Replay& yrp, const Options& opt,
				   bool instrument, const LineConstraints* cons = nullptr) {
	LineResult r;
	auto t0 = Clock::now();
	Arena* arena = duel.GetArena();
	uint32_t phase = 0;
	bool first_idle_seen = false;
	// Suivi du prompt courant, seulement si des contraintes sont a juger — ou si
	// le harnais d'operateurs releve les activations (session 19).
	const bool track = cons && cons->Any();
	const bool need_prompt = track || opt.operators;
	uint8_t ptype = 0;
	int pplayer = -1;
	std::vector<uint8_t> ppayload;
	EnumOptions peo;
	if(need_prompt) {
		if(track)
			peo.no_activate =
				cons->no_activate.empty() ? nullptr : &cons->no_activate;
		peo.db = &duel.Db();
	}

	// Capture du BOARD CIBLE. Deux instants possibles, et c'est le second qui
	// manquait : (1) le changement de tour, quand la ligne enregistree le
	// franchit ; (2) LA FIN DE L'ENREGISTREMENT. Un hand test qui s'arrete une
	// fois le combo pose est un replay parfaitement valide — c'est meme la
	// facon normale d'enregistrer une ligne — et exiger qu'il passe son tour
	// etait une hypothese de cet outil, pas une propriete des replays. Sans ce
	// repli, un tel replay ne rendait aucune cible, donc aucun plan, et le flux
	// de transplantation refusait de demarrer.
	auto capture_target = [&] {
		r.target_self = Snapshot(duel, uint8_t(opt.target_player));
		r.target_oppo = Snapshot(duel, uint8_t(1 - opt.target_player));
		r.target_at = r.responses_used;
		r.fingerprint_at_target = Fingerprint(duel);
		r.summons_at_target = r.summon_codes.size();
		r.burned_at_target =
			duel.Count(uint8_t(opt.target_player), LOCATION_GRAVE) +
			duel.Count(uint8_t(opt.target_player), LOCATION_REMOVED);
		r.have_target = true;
	};

	if(instrument && arena)
		arena->ResetDirtyTracking();

	for(;;) {
		int status = duel.Process();
		for(const Message& m : duel.Messages()) {
			switch(m.type) {
			case MSG_NEW_TURN:
				++r.turns;
				// Fin du tour du joueur cible : instant ou le board cible est
				// defini (cf. section 4 du document de conception).
				if(r.turns == 2 && !r.have_target)
					capture_target();
				break;
			case MSG_NEW_PHASE:
				if(m.size >= 2) { uint16_t p = 0; std::memcpy(&p, m.data, 2); phase = p; }
				break;
			case MSG_SUMMONING:
			case MSG_SPSUMMONING:
				if(m.type == MSG_SUMMONING)
					++r.summon;
				else
					++r.spsummon;
				if(m.size >= 4) {
					uint32_t c = 0;
					std::memcpy(&c, m.data, 4);
					r.summon_codes.push_back(c);
					// Invocations surveillees (--summon-min), comptees
					// jusqu'au board comme les resolutions.
					if(track && !cons->resolve_min.empty() && !r.have_target &&
					   c) {
						const uint32_t sc = duel.Db().Canonical(c);
						r.resolve_counts.resize(cons->resolve_min.size(), 0);
						for(size_t i = 0; i < cons->resolve_min.size(); ++i)
							if(cons->resolve_min[i].on_summon &&
							   cons->resolve_min[i].code == sc)
								++r.resolve_counts[i];
					}
				}
				break;
			case MSG_FLIPSUMMONING: ++r.flipsummon; break;
			case MSG_CHAINING:
				++r.chaining;
				// La sequence des chaines, lisible : c'est elle qui dit QUI
				// contre QUOI (le test adverse --fire produit des replays ou
				// la question « qui a nege Nibiru ? » se lit ici).
				if(opt.verbose && m.size >= 4) {
					uint32_t vc = 0;
					std::memcpy(&vc, m.data, 4);
					std::printf("      [chaine %lld] %s  (activation %s)\n",
								r.chaining,
								duel.Db().Name(duel.Db().Canonical(vc)).c_str(),
								ZoneMaskName(ChainingLocation(m.data, m.size))
									.c_str());
				}
				if(track && !cons->resolve_min.empty() && !r.have_target &&
				   m.size >= 4) {
					uint32_t c = 0;
					std::memcpy(&c, m.data, 4);
					c = duel.Db().Canonical(c);
					const uint32_t loc = ChainingLocation(m.data, m.size);
					r.resolve_counts.resize(cons->resolve_min.size(), 0);
					for(size_t i = 0; i < cons->resolve_min.size(); ++i)
						if(!cons->resolve_min[i].on_summon &&
						   cons->resolve_min[i].code == c &&
						   (!cons->resolve_min[i].zones ||
							(loc & cons->resolve_min[i].zones)))
							++r.resolve_counts[i];
				}
				break;
			case MSG_RETRY:         ++r.retries; break;
			default: break;
			}

			if(need_prompt && IsPrompt(m.type)) {
				ptype = m.type;
				pplayer = m.size ? m.data[0] : -1;
				ppayload.assign(m.data, m.data + m.size);
			}
			if(!instrument || !IsPrompt(m.type))
				continue;
			PromptInfo info = DecodePrompt(m.type, m.data, m.size);
			Stat& s = r.stats[m.type];
			++s.n;
			if(info.raw < 0) {
				++s.undecoded;
			} else {
				if(info.dedup <= 1) ++s.forced;
				else if(info.dedup <= 2) ++s.binary;
				s.raw_sum += info.raw;
				s.dedup_sum += info.dedup;
				s.raw_max = (std::max)(s.raw_max, info.raw);
				s.dedup_max = (std::max)(s.dedup_max, info.dedup);
				if(info.raw > 0) r.log_raw += std::log10(double(info.raw));
				if(info.dedup > 0) r.log_dedup += std::log10(double(info.dedup));
			}
			if(opt.verbose)
				std::printf("  #%-4zu T%d %-8s %-22s brut=%-10.0Lf dedup=%-8.0Lf %s\n",
							r.responses_used, r.turns, PhaseName(phase),
							PromptName(m.type), info.raw, info.dedup,
							info.detail.c_str());
		}

		if(status == OCG_DUEL_STATUS_AWAITING) {
			if(!r.have_start) {
				r.start_self = Snapshot(duel, uint8_t(opt.target_player));
				r.have_start = true;
			}
			// Pic de brulees en cours de ligne, jusqu'au board : la mesure qui
			// calibre la marge de la borne B&B (--burn-slack).
			if(!r.have_target) {
				const uint32_t b =
					duel.Count(uint8_t(opt.target_player), LOCATION_GRAVE) +
					duel.Count(uint8_t(opt.target_player), LOCATION_REMOVED);
				if(b > r.burned_max)
					r.burned_max = b;
			}
			// Jugement de la reference contre --guard (aux fenetres adverses,
			// la ou Nibiru tomberait) et --no-activate (reponse enregistree).
			if(track && !r.have_target) {
				if(!cons->guard.empty() &&
				   pplayer == 1 - opt.target_player &&
				   r.summon_codes.size() >= cons->guard_after) {
					uint32_t opp_hand = duel.Count(
						static_cast<uint8_t>(1 - opt.target_player),
						LOCATION_HAND);
					// Menace eteinte (handrip accompli) : fenetre hors sujet.
					bool threat = cons->guard_opp_hand_release < 0 ||
								  static_cast<int>(opp_hand) >
									  cons->guard_opp_hand_release;
					if(threat) {
						++r.guard_checks;
						BoardKey fk = ComputeBoardKey(
							duel, static_cast<uint8_t>(opt.target_player));
						if(!GuardHolds(duel,
									   static_cast<uint8_t>(opt.target_player),
									   cons->guard, fk.codes)) {
							if(!r.guard_violations) {
								r.first_guard_violation_summon =
									r.summon_codes.size();
								r.first_guard_violation_opp_hand = opp_hand;
							}
							++r.guard_violations;
						}
					}
				}
				if(peo.no_activate && r.responses_used < yrp.responses.size() &&
				   ResponseForbidden(ptype, ppayload.data(),
									 static_cast<uint32_t>(ppayload.size()),
									 yrp.responses[r.responses_used], peo))
					++r.forbidden_activations;
			}
			if(instrument && arena) {
				// Pages salies pour avancer d'UNE decision : c'est ce que
				// couterait un instantane incremental par noeud explore.
				auto t = Clock::now();
				size_t pages = arena->CountDirtyPages();
				r.ms_write_watch += MsSince(t);
				++r.write_watch_calls;
				if(first_idle_seen)   // on ignore la mise en place initiale
					r.dirty_per_decision.push_back(pages);
				first_idle_seen = true;
			}
			if(r.responses_used >= yrp.responses.size())
				break;   // fin de l'enregistrement : le joueur a quitte
			// HARNAIS D'OPERATEURS : ce que la reponse ENREGISTREE active.
			// Seules les decisions du joueur cible comptent — un operateur de
			// l'adversaire ne serait pas dans la table (elle est batie sur NOTRE
			// deck) et compterait a tort en « non appariee ».
			if(opt.operators && pplayer == opt.target_player) {
				ActivationRead ar;
				if(DecodeActivation(ptype, ppayload.data(),
									static_cast<uint32_t>(ppayload.size()),
									yrp.responses[r.responses_used], peo, ar)) {
					ObservedActivation oa;
					oa.at = r.responses_used;
					oa.message = ptype;
					oa.code = ar.code;
					oa.desc = ar.desc;
					oa.location = ar.has_zone ? ar.location : uint8_t(0);
					oa.sequence = ar.sequence;
					oa.turn = r.turns;
					r.activations.push_back(oa);
				}
			}
			duel.SetResponse(yrp.responses[r.responses_used]);
			++r.responses_used;
		} else if(status == OCG_DUEL_STATUS_END) {
			break;
		} else if(status != OCG_DUEL_STATUS_CONTINUE) {
			std::printf("\n!! statut de duel inattendu : %d\n", status);
			break;
		}
	}
	// Repli : la ligne s'arrete sans changement de tour (le joueur a quitte une
	// fois son board pose). L'etat final EST le board cible.
	if(!r.have_target && r.responses_used) {
		capture_target();
		r.target_at_is_end = true;
	}
	r.fingerprint_final = Fingerprint(duel);
	r.ms = MsSince(t0);
	return r;
}

void ReportLine(const LineResult& r, const Replay& yrp, const CardDB& db,
				const Options& opt) {
	std::printf("\n=== resultats ===\n");
	std::printf("  reponses consommees : %zu / %zu\n", r.responses_used,
				yrp.responses.size());
	std::printf("  MSG_RETRY           : %zu   %s\n", r.retries,
				r.retries ? "<-- REJEU DIVERGENT, mesures invalides"
						  : "(rejeu fidele)");
	std::printf("  tours joues         : %d\n", r.turns);
	if(r.have_target)
		std::printf("  brulees             : %u au board, pic %u en cours de "
					"ligne (marge de recuperation %d — calibre --burn-slack)\n",
					r.burned_at_target, r.burned_max,
					static_cast<int>(r.burned_max) -
						static_cast<int>(r.burned_at_target));
	else if(r.burned_max)
		// Une ligne de solution s'arrete au tour 1 sans capturer de cible :
		// son pic reste la donnee qui calibre --burn-slack (session 6 : la
		// marge doit couvrir la recuperation de la MEILLEURE ligne, pas
		// seulement celle de la reference).
		std::printf("  brulees             : pic %u en cours de ligne (pas de "
					"board cible capture)\n", r.burned_max);

	int total = 0, forced = 0, binary = 0, decoded = 0;
	for(const auto& [type, s] : r.stats) {
		total += s.n;
		forced += s.forced;
		binary += s.binary;
		decoded += s.n - s.undecoded;
	}
	std::vector<std::pair<uint8_t, Stat>> ordered(r.stats.begin(), r.stats.end());
	std::sort(ordered.begin(), ordered.end(),
			  [](const auto& a, const auto& b) { return a.second.n > b.second.n; });

	std::printf("\n--- points de decision par type ---\n");
	std::printf("  %-24s%6s%12s%11s%12s%11s\n", "type", "n", "brut moy",
				"brut max", "dedup moy", "dedup max");
	for(const auto& [type, s] : ordered) {
		int d = s.n - s.undecoded;
		if(d > 0)
			std::printf("  %-24s%6d%12.1Lf%11.0Lf%11.1Lf%11.0Lf\n", PromptName(type),
						s.n, s.raw_sum / d, s.raw_max, s.dedup_sum / d, s.dedup_max);
		else
			std::printf("  %-24s%6d%12s\n", PromptName(type), s.n, "non decode");
	}
	std::printf("  %-24s%6d\n", "TOTAL", total);

	std::printf("\n--- potentiel d'elision (une seule reponse legale) ---\n");
	for(const auto& [type, s] : ordered) {
		int d = s.n - s.undecoded;
		if(d > 0 && s.forced)
			std::printf("  %-24s %4d / %-4d forcees  (%.0f%%)\n", PromptName(type),
						s.forced, d, 100.0 * s.forced / d);
	}
	std::printf("  %-24s %4d / %-4d forcees  (%.0f%%)\n", "TOTAL", forced, decoded,
				decoded ? 100.0 * forced / decoded : 0.0);
	std::printf("  => profondeur apres elision : %d au lieu de %d\n",
				decoded - forced, decoded);

	std::printf("\n--- ordre de grandeur du branchement le long de CETTE ligne ---\n");
	std::printf("  produit des branchements bruts : 10^%.1Lf\n", r.log_raw);
	std::printf("  apres dedup par code           : 10^%.1Lf\n", r.log_dedup);
	std::printf("  (indicateur d'echelle, PAS un decompte de feuilles : changer un\n"
				"   choix precoce modifie les prompts suivants.)\n");

	if(r.have_start) {
		std::printf("\n--- position de depart (joueur %d) ---\n", opt.target_player);
		for(const auto& c : r.start_self.hand_cards)
			if(c.present)
				std::printf("      MAIN      %9u  %s\n", c.Code(),
							db.Name(c.Code()).c_str());
		std::printf("      DECK=%u  EXTRA=%u\n", r.start_self.deck,
					r.start_self.extra);
	}

	std::printf("\n--- cout de la ligne de reference ---\n");
	std::printf("  invocations normales   : %lld\n", r.summon);
	std::printf("  invocations speciales  : %lld\n", r.spsummon);
	std::printf("  invocations flip       : %lld\n", r.flipsummon);
	std::printf("  activations            : %lld\n", r.chaining);
	std::printf("  A_ref (tier 2)         : %lld\n",
				r.summon + r.spsummon + r.flipsummon + r.chaining);
	std::printf("  D_ref (tier 3)         : %zu\n", r.responses_used);

	if(r.have_target) {
		const Deck& deck = yrp.decks[opt.target_player];
		size_t owned = deck.main.size() + deck.extra.size();
		size_t left = r.target_self.hand + r.target_self.deck + r.target_self.extra;
		size_t on_board = CountPresent(r.target_self.mzone) +
						  CountPresent(r.target_self.szone);
		size_t burned = r.target_self.grave + r.target_self.removed;
		std::printf("\n--- board cible (%s, joueur %d) ---\n",
					r.target_at_is_end
						? "FIN DE L'ENREGISTREMENT : la ligne ne passe pas le tour"
						: "fin du tour",
					opt.target_player);
		std::printf("    capture apres la reponse #%zu\n", r.target_at);
		PrintBoard(r.target_self, db);
		std::printf("\n  C_ref (tier 1) = cartes hors main/deck/extra\n");
		std::printf("    total joueur %d        : %zu\n", opt.target_player, owned);
		std::printf("    restant main+deck+xtra: %zu\n", left);
		std::printf("    => consommees         : %zu\n", owned - left);
		std::printf("       dont sur le board  : %zu  (constant : impose par le "
					"critere d'equivalence)\n", on_board);
		std::printf("       dont brulees GY/ban: %zu  <-- c'est CELA que le "
					"solveur doit minimiser\n", burned);

		// Liste nominative des cartes consommees. Sans elle on ne peut pas dire
		// si un AUTRE deck a de quoi refaire ce board : seul le detail permet de
		// confronter la depense de la ligne au contenu d'un deck different.
		std::map<uint32_t, uint32_t> spent;
		for(const auto* zone : { &r.target_self.grave_cards,
								 &r.target_self.removed_cards,
								 &r.target_self.mzone, &r.target_self.szone })
			for(const auto& [code, n] : CodeCounts(*zone, db))
				spent[code] += n;
		std::printf("\n--- cartes engagees par la ligne (board + GY + bannies) ---\n");
		for(const auto& [code, n] : spent)
			std::printf("      %dx %9u  %s\n", n, code, db.Name(code).c_str());
	} else {
		std::printf("\n!! le tour du joueur cible ne s'est pas termine\n");
	}
}

void ReportDirty(const LineResult& r, const Arena& arena, double ms_per_decision) {
	if(r.dirty_per_decision.empty())
		return;
	std::vector<size_t> v = r.dirty_per_decision;
	std::sort(v.begin(), v.end());
	size_t sum = 0;
	for(size_t x : v) sum += x;
	size_t page = arena.PageSize();
	auto pct = [&](double q) { return v[(std::min)(v.size() - 1,
												   size_t(q * v.size()))]; };
	double avg = double(sum) / v.size();
	auto kb = [&](double pages) { return pages * page / 1024.0; };
	std::printf("\n--- pages salies pour avancer d'UNE decision (%zu mesures) ---\n",
				v.size());
	std::printf("  mediane : %6zu pages  (%7.0f Ko)\n", pct(0.5), kb(double(pct(0.5))));
	std::printf("  moyenne : %6.0f pages  (%7.0f Ko)\n", avg, kb(avg));
	std::printf("  p90     : %6zu pages  (%7.0f Ko)\n", pct(0.9), kb(double(pct(0.9))));
	std::printf("  max     : %6zu pages  (%7.0f Ko)\n", v.back(), kb(double(v.back())));

	// Un instantane incremental copie la page sale deux fois (journal + miroir)
	// et une restauration une fois. A ~10 Go/s de bande passante memoire.
	double bytes_push = 2.0 * avg * page, bytes_pop = avg * page;
	double ms_push = bytes_push / 10e9 * 1000.0, ms_pop = bytes_pop / 10e9 * 1000.0;
	std::printf("\n  projection d'un instantane incremental, par noeud explore :\n");
	std::printf("    copie a l'empilement  : %7.0f Ko -> %.3f ms\n",
				bytes_push / 1024.0, ms_push);
	std::printf("    copie a la restauration: %6.0f Ko -> %.3f ms\n",
				bytes_pop / 1024.0, ms_pop);
	if(r.write_watch_calls) {
		double ww = r.ms_write_watch / r.write_watch_calls;
		std::printf("    GetWriteWatch mesure   : %.3f ms par appel, 2 appels/noeud\n", ww);
		double total = ms_push + ms_pop + 2 * ww;
		std::printf("    total instantane       : %.3f ms  contre %.3f ms de travail"
					"  => %.0f%% de surcout\n", total, ms_per_decision,
					ms_per_decision > 0 ? 100.0 * total / ms_per_decision : 0.0);
	}
}

// Avance le duel d'exactement `n` decisions a partir de la reponse `from`.
// Renvoie le nombre de decisions reellement consommees.
// `chain_codes` (s24, chantier 4) : recoit le code BRUT de chaque activation
// (MSG_CHAINING) rencontree — la meme observation que quota_uses dans la
// recherche, pour que la marche du theoreme 2 compte les quotas du chemin
// avec la comptabilite du run.
size_t Advance(Duel& duel, const Replay& yrp, size_t from, size_t n,
			   uint32_t* actions = nullptr,
			   std::vector<uint32_t>* chain_codes = nullptr) {
	size_t used = 0;
	while(used < n) {
		int status = duel.Process();
		for(const Message& m : duel.Messages()) {
			if(actions && (m.type == MSG_SUMMONING || m.type == MSG_SPSUMMONING ||
						   m.type == MSG_FLIPSUMMONING || m.type == MSG_CHAINING))
				++*actions;
			if(chain_codes && m.type == MSG_CHAINING && m.size >= 4) {
				uint32_t cc = 0;
				std::memcpy(&cc, m.data, 4);
				chain_codes->push_back(cc);
			}
		}
		if(status == OCG_DUEL_STATUS_AWAITING) {
			if(from + used >= yrp.responses.size())
				break;
			duel.SetResponse(yrp.responses[from + used]);
			++used;
		} else if(status == OCG_DUEL_STATUS_END) {
			break;
		}
	}
	return used;
}

// MESURE DE LARGEUR EFFECTIVE — le prealable a tout elagage par nouveaute.
//
// Iterated Width ne garde un etat que s'il rend vrai un fait inedit. Avant
// d'elaguer quoi que ce soit, il faut savoir si la LIGNE DE REFERENCE
// elle-meme survivrait : les resolutions de chaine passent par des etats
// "muets" qui ne changent rien au board, et les couper au premier silence
// tuerait la seule solution connue. On mesure donc, decision par decision, si
// l'etat produit un atome neuf, et la plus longue serie muette — c'est elle
// qui fixe la patience de l'elagage. Une largeur qui ne se mesure pas ne se
// promet pas.
uint32_t MeasureWidth(Duel& duel, const Replay& yrp, const Options& opt,
					  Arena& arena, const LineResult& ref) {
	std::printf("\n=== largeur effective (atomes IW) le long de la reference ===\n");
	while(arena.Depth() > 1)
		arena.Pop();
	if(arena.Depth() == 0)
		arena.Push();
	arena.Restore();

	const auto con = static_cast<uint8_t>(opt.target_player);
	BoardKey target;
	{
		size_t at = 0;
		at += Advance(duel, yrp, at, ref.target_at);
		target = ComputeBoardKey(duel, con);
		arena.Restore();
	}

	NoveltyTable flat, serial;
	std::vector<uint64_t> atoms;
	uint32_t states = 0;
	uint32_t mute_flat = 0, run_flat = 0, max_flat = 0;
	uint32_t mute_serial = 0, run_serial = 0, max_serial = 0;
	size_t at = 0;
	while(at < ref.target_at) {
		size_t used = Advance(duel, yrp, at, 1);
		if(!used)
			break;
		at += used;
		++states;
		BoardKey here = ComputeBoardKey(duel, con);
		CollectAtoms(duel, con, here, 0, atoms);
		if(flat.Observe(atoms, static_cast<uint32_t>(at)))
			run_flat = 0;
		else { ++mute_flat; max_flat = (std::max)(max_flat, ++run_flat); }
		// Partition par sous-but atteint : la table se rouvre a chaque carte
		// cible posee (serialisation du but conjonctif).
		uint32_t part = CommonCodes(here.codes, target.codes);
		CollectAtoms(duel, con, here, part, atoms);
		if(serial.Observe(atoms, static_cast<uint32_t>(at)))
			run_serial = 0;
		else { ++mute_serial; max_serial = (std::max)(max_serial, ++run_serial); }
	}
	arena.Restore();

	std::printf("  etats visites            : %u\n", states);
	std::printf("  atomes distincts         : %zu  (%zu avec serialisation)\n",
				flat.Size(), serial.Size());
	std::printf("  etats muets (rien de neuf): %u (%.0f%%)  |  serialise : %u (%.0f%%)\n",
				mute_flat, states ? 100.0 * mute_flat / states : 0.0,
				mute_serial, states ? 100.0 * mute_serial / states : 0.0);
	std::printf("  plus longue serie muette : %u  |  serialise : %u\n",
				max_flat, max_serial);

	// La patience doit couvrir la plus longue serie muette de la ligne connue,
	// avec une marge : un autre deck peut etre un peu plus bavard en silences.
	uint32_t patience = (std::max)(12u, max_serial + 4u);
	if(opt.novelty >= 0)
		patience = static_cast<uint32_t>(opt.novelty);
	std::printf("  => patience %s : %u decisions%s\n",
				opt.novelty >= 0 ? "imposee" : "retenue", patience,
				patience == 0 ? "  (elagage DESACTIVE)" : "");
	if(max_flat + 4 > patience && patience)
		std::printf("     (une ligne non serialisee aurait demande %u : la\n"
					"      serialisation du but reduit le silence)\n", max_flat + 4);
	return patience;
}

// --- UN SEUL POINT DE CABLAGE (session 20, chantier D) -----------------------
//
// LE FAIT QUI JUSTIFIE CETTE FONCTION, et il est mesure. Trois `SearchConfig`
// etaient construits a trois endroits de ce fichier ; un releve champ par champ
// rend, sur 82 champs : `RunTransplantSolve` en cable 66, `RunSolve` 11,
// `RunGrowth` 6. Ce n'est PAS une divergence de propos — les budgets different
// legitimement d'un mode a l'autre — c'est que les MECANISMES choisis en ligne
// de commande n'etaient appliques que sur UN chemin :
//
//   * `--elide-forced` a passe TROIS sessions de bancs sur le chemin de
//     recherche ou il ne faisait rien (9.28 (f), preuve a l'octet pres) ;
//   * le mode `--solve` — le CONTROLE DE SANTE — n'a jamais vu ni
//     `--hindsight` ni `--adapt-to-peak`, ce qui explique que la sante soit
//     restee identique a travers leur promotion ;
//   * `--max-rollouts` / `--max-nodes`, l'instrument du mode DETERMINISTE,
//     n'existaient pas hors transplantation.
//
// La regle est desormais mecanique : TOUT champ de `SearchConfig` qui vient
// d'une option est assigne ICI, et nulle part ailleurs. Restent a l'appelant, et
// seulement eux :
//   - les BUDGETS propres a un mode (bornes issues de la ligne de reference,
//     profondeur de `--growth`, `anytime` de `--optimize`) ;
//   - les POINTEURS vers des objets locaux (graphe de recettes, catalogue
//     d'options, table partagee, politique NRPA) : ils n'existent pas dans tous
//     les modes, et c'est precisement ce que `ReportMechanisms` rend visible ;
//   - la derivation NRPA PAR WORKER (`nrpa_level` depend du nombre de fils),
//     seule exception assumee, et elle est locale a la phase tirages.
void ApplyMechanisms(const Options& opt, SearchConfig& cfg) {
	cfg.target_player = opt.target_player;

	// Budget en COMPTE (mode deterministe). `max_rollouts` a 0 = pas de borne,
	// donc l'assignation inconditionnelle est neutre ; `max_nodes` a 0 signifie
	// « laisse le mode choisir », d'ou la garde.
	cfg.max_rollouts = opt.max_rollouts;
	if(opt.max_nodes)
		cfg.max_nodes = opt.max_nodes;
	// Le domaine en TOURS (s22quater) : --turns 2 laisse la ligne traverser
	// le tour adverse (le 2e rip d'Omega y vit). 0 = defaut = 1 tour.
	cfg.max_turns = opt.turns ? static_cast<uint32_t>(opt.turns) : 1u;

	// Les quatre leviers de la session 17 et leurs suites. Tous sont LUS sous
	// garde de `cfg.recipes` (search.cpp:2556) : les cabler sans graphe est sur
	// et inerte — et `ReportMechanisms` le DIT au lieu de le taire.
	cfg.assign = opt.assign;
	cfg.assign_bias = static_cast<float>(opt.assign_bias);
	cfg.op_bias = static_cast<float>(opt.op_bias);
	cfg.backward = opt.backward;
	cfg.probe_repeat = opt.probe_repeat;
	if(opt.recipes >= 0)
		cfg.recipe_h = static_cast<float>(opt.recipes);
	cfg.landmark_weight = static_cast<float>(opt.landmark_weight);
	cfg.landmark_h = static_cast<float>(opt.landmark_h);

	cfg.hindsight = static_cast<float>(opt.hindsight);
	cfg.hindsight_k = opt.hindsight_k;
	cfg.adapt_to_peak = opt.adapt_to_peak;
	cfg.elide_forced = opt.elide_forced;
	cfg.resolve_weight = static_cast<float>(opt.resolve_weight);

	// Finisseur et parcours.
	cfg.levin_h = static_cast<float>(opt.levin_h);
	cfg.levin_reroot = opt.levin_reroot;
	cfg.reroot_h = static_cast<float>(opt.reroot_h);
	cfg.dive_full = opt.dive_full;
	cfg.lifo_ties = opt.lifo_ties;
	cfg.merged_pop = opt.merged_pop;
	cfg.finisher_post_goal = opt.finisher_post_goal;
	cfg.finisher_options = opt.finisher_options;
	cfg.archive_k = opt.archive_k;

	// Politique. `-1` est le sentinelle « non donne » : le defaut du moteur
	// gagne, et c'est ce que le rapport imprime.
	if(opt.hint_bias >= 0)
		cfg.hint_bias = static_cast<float>(opt.hint_bias);
	if(opt.nrpa_bias >= 0)
		cfg.nrpa_bias_known = static_cast<float>(opt.nrpa_bias);
	if(opt.nrpa_alpha > 0)
		cfg.nrpa_alpha = static_cast<float>(opt.nrpa_alpha);
	if(opt.nrpa_iters)
		cfg.nrpa_iters = opt.nrpa_iters;
	cfg.nrpa_restart_keep = static_cast<float>(opt.nrpa_keep);
	cfg.nrpa_temp = static_cast<float>(opt.nrpa_temp);
	cfg.nrpa_adapt_passes = opt.adapt_passes;
	cfg.ctx_shrink = static_cast<float>(opt.ctx_shrink);
	cfg.ctx_max = opt.ctx_max;
	cfg.qhat_depth = opt.qhat_depth;
	cfg.qhat_window = opt.qhat_window;
	cfg.qhat_rho = opt.qhat_rho;
	cfg.qhat_max_nodes = opt.qhat_nodes;

	// Cout lexicographique des brulees. Les valeurs par defaut d'`Options` et de
	// `SearchConfig` coincident : l'assignation ne mord que si le drapeau est
	// passe, et elle mord desormais dans TOUS les modes, pas seulement sous
	// `--optimize`.
	cfg.burn_slack = opt.burn_slack;
	cfg.burn_limit = opt.burn_limit;
	cfg.phase_w = static_cast<float>(opt.phase_w);
	// Retour au barreau (s21). Lu sous garde de serial_reqs (search.cpp) :
	// le cabler sans echelle est sur et inerte — et ReportMechanisms le DIT.
	cfg.reenter = static_cast<float>(opt.reenter);
	// Quotas du chemin dans le LP (s24, chantier 4). Ne mord qu'au
	// raffinement — sans refine_after ni modele, il est inerte, et
	// ReportMechanisms le DIT.
	cfg.quota_h = opt.quota_h;
}

// LE CONTROLE QUI MANQUAIT, et il est la vraie lecon de 9.28 (f).
//
// « Un mecanisme doit imprimer sa vie » ne suffisait pas : quand le champ
// n'etait cable NULLE PART, il n'y avait tout simplement RIEN a imprimer, et le
// banc lisait un silence comme une absence d'effet. Cette fonction se lit APRES
// que l'appelant a branche ses pointeurs, et rend deux choses qu'aucun log ne
// rendait : les mecanismes ACTIFS dans ce mode, et ceux qui sont DEMANDES mais
// INERTES ici faute de dependance. Un bras de mesure dont le rapport porte une
// ligne `!! INERTE` est un bras a jeter avant de le lancer, pas apres.
void ReportMechanisms(const SearchConfig& cfg, const char* mode) {
	const SearchConfig d;   // les defauts, pour n'imprimer que les ecarts
	std::string on, inert;
	char buf[160];
	auto add = [&](std::string& dst, const char* fmt, auto... args) {
		std::snprintf(buf, sizeof(buf), fmt, args...);
		if(!dst.empty())
			dst += ", ";
		dst += buf;
	};

	if(cfg.elide_forced != d.elide_forced)     add(on, "elide-forced");
	if(cfg.adapt_to_peak != d.adapt_to_peak)   add(on, "adapt-to-peak");
	if(cfg.hindsight != d.hindsight)           add(on, "hindsight %.2f", cfg.hindsight);
	if(cfg.assign != d.assign)                 add(on, "assign");
	if(cfg.resolve_weight != d.resolve_weight) add(on, "resolve-w %.0f", cfg.resolve_weight);
	if(cfg.max_rollouts)                       add(on, "max-rollouts %llu",
												   (unsigned long long)cfg.max_rollouts);
	if(cfg.hint_bias != d.hint_bias)           add(on, "hint-bias %.2f", cfg.hint_bias);
	if(cfg.qhat_depth != d.qhat_depth)         add(on, "qhat %u", cfg.qhat_depth);

	// Les cinq qui EXIGENT le graphe de recettes, et les deux qui exigent les
	// landmarks. Un poids non nul sans son pointeur est exactement le piege 42.
	auto dep = [&](bool served, const char* fmt, auto... args) {
		add(served ? on : inert, fmt, args...);
	};
	if(cfg.assign_bias > 0.0f)
		dep(cfg.recipes != nullptr, "assign-bias %.2f", cfg.assign_bias);
	if(cfg.op_bias > 0.0f)
		dep(cfg.recipes != nullptr, "op-bias %.2f", cfg.op_bias);
	if(cfg.backward)
		dep(cfg.recipes != nullptr, "backward");
	if(cfg.probe_repeat)
		dep(cfg.recipes != nullptr, "probe-repeat");
	if(cfg.recipe_h > 0.0f)
		dep(cfg.recipes != nullptr, "recipe-h %.2f", cfg.recipe_h);
	if(cfg.landmark_weight > 0.0f)
		dep(cfg.landmarks != nullptr, "landmark-w %.2f", cfg.landmark_weight);
	if(cfg.landmark_h > 0.0f)
		dep(cfg.landmarks != nullptr, "landmark-h %.2f", cfg.landmark_h);
	// Le retour au barreau exige l'ECHELLE (serial_reqs) : sans elle, une
	// cellule d'archive est un cache, pas un barreau — et le mecanisme est
	// volontairement inerte. Le dire ici evite un bras d'A/B mort-ne.
	if(cfg.reenter > 0.0f)
		dep(!cfg.serial_reqs.empty(), "reenter %.2f", cfg.reenter);
	// Le raffinement (s22) exige l'echelle ET le modele de bilan prete.
	if(cfg.refine_after)
		dep(!cfg.serial_reqs.empty() && cfg.balance != nullptr,
			"refine-after %u", cfg.refine_after);
	// Les quotas du chemin (s24) ne mordent QU'AU raffinement : sans lui (ou
	// sans hotes a quota derives), le drapeau est demande mais inerte.
	if(cfg.quota_h)
		dep(cfg.refine_after != 0 && !cfg.serial_reqs.empty() &&
				cfg.balance != nullptr && !cfg.quota_hosts.empty(),
			"quota-h");

	std::printf("  MECANISMES [%s] : %s\n", mode,
				on.empty() ? "aucun (defauts du moteur)" : on.c_str());
	if(!inert.empty())
		std::printf("!! DEMANDE mais INERTE ici (dependance absente dans ce "
					"mode) : %s\n", inert.c_str());
}

// Le test unique ne prouve qu'une chose : Push/Pop marche a profondeur 1. Un
// mecanisme de journal casse plutot sur les sequences imbriquees, les freres
// successifs et les restaurations repetees. On les exerce ici tout au long de
// la ligne, en verifiant a chaque etape que l'etat revient bien a l'identique.
int RunStressTest(Duel& duel, const Replay& yrp, const Options& opt, Arena& arena,
				  uint64_t expected_final) {
	// Invariant d'entree ET de sortie de chaque cas : profondeur 1, duel au
	// debut de la ligne. On y revient par Restore(), qui conserve le niveau.
	std::printf("\n=== test de stress des instantanes ===\n");
	while(arena.Depth() > 1)
		arena.Pop();
	if(arena.Depth() == 0)
		arena.Push();
	arena.Restore();

	struct Case { const char* name; int failures; int checks; };
	std::vector<Case> cases;
	auto t0 = Clock::now();

	// --- 1. freres successifs : Push, avancer, Restore, re-avancer, comparer
	{
		int fail = 0, checks = 0;
		size_t at = 0;
		uint64_t ref = Fingerprint(duel);
		for(int step = 0; step < 12 && at + 8 < yrp.responses.size(); ++step) {
			if(opt.verbose)
				std::printf("    [freres] etape %d, decision %zu, profondeur %zu\n",
							step, at, arena.Depth());
			arena.Push();
			size_t k = Advance(duel, yrp, at, 8);
			uint64_t a = Fingerprint(duel);
			arena.Restore();
			if(Fingerprint(duel) != ref) { ++fail; }
			++checks;
			size_t k2 = Advance(duel, yrp, at, 8);
			uint64_t b = Fingerprint(duel);
			if(a != b || k != k2) { ++fail; }
			++checks;
			arena.Pop();
			if(Fingerprint(duel) != ref) { ++fail; }
			++checks;
			// avancer pour de bon
			at += Advance(duel, yrp, at, 8);
			ref = Fingerprint(duel);
		}
		while(arena.Depth() > 1)
			arena.Pop();
		arena.Restore();
		cases.push_back({ "freres successifs (Push/Restore/Pop)", fail, checks });
	}

	// --- 2. imbrication profonde : empiler N niveaux puis tout depiler
	{
		int fail = 0, checks = 0;
		std::vector<uint64_t> refs;
		size_t at = 0;
		for(int depth = 0; depth < 20 && at + 6 < yrp.responses.size(); ++depth) {
			refs.push_back(Fingerprint(duel));
			arena.Push();
			at += Advance(duel, yrp, at, 6);
		}
		while(!refs.empty()) {
			arena.Pop();
			if(Fingerprint(duel) != refs.back()) ++fail;
			++checks;
			refs.pop_back();
		}
		while(arena.Depth() > 1)
			arena.Pop();
		arena.Restore();
		cases.push_back({ "imbrication profonde (20 niveaux)", fail, checks });
	}

	// --- 3. le duel doit rester jouable jusqu'au bout apres tout ca
	{
		int fail = 0;
		LineResult full = RunLine(duel, yrp, opt, false);
		if(full.retries || full.fingerprint_final != expected_final)
			++fail;
		arena.Restore();
		cases.push_back({ "ligne complete rejouee apres stress", fail, 1 });
	}

	double ms = MsSince(t0);
	int total_fail = 0;
	for(const auto& c : cases) {
		std::printf("  %-42s %3d/%-3d %s\n", c.name, c.checks - c.failures,
					c.checks, c.failures ? "<-- ECHEC" : "ok");
		total_fail += c.failures;
	}
	std::printf("  duree du test : %.0f ms\n", ms);
	std::printf("  => %s\n", total_fail == 0
		? "les instantanes resistent a l'imbrication et aux restaurations repetees"
		: "DEFAUT dans le mecanisme d'instantane");
	return total_fail ? 1 : 0;
}

// VALIDATION DE L'ENUMERATEUR.
//
// Definis plus bas, avec le pilote de transplantation.
// `opp_hand` : cartes ajoutees a la main adverse du duel de VERIFICATION —
// nul en meme-deck (la reference n'en a pas), celui de la recherche en
// transplantation : la verification doit rejouer le MEME duel que la recherche.
size_t WriteSolutions(const std::vector<Solution>& sols, const Replay& start_yrp,
					  const BoardKey& target, const Options& opt, CardDB& db,
					  ScriptProvider& scripts, const std::string& outdir,
					  const LineConstraints& cons,
					  const std::vector<uint32_t>* opp_hand = nullptr,
					  const std::vector<BoardKey>* target_alts = nullptr);

// Verdict de la reference face aux contraintes de ligne, et sa sequence
// d'invocations — c'est elle qui permet de choisir le "n" d'une contrainte.
// Rend false si la reference viole quelque chose : le controle "a zero ecart
// la reference est retrouvee" est alors suspendu, par construction et non par
// defaut.
bool ReportConstraints(const LineConstraints& cons, const LineResult& ref,
					   const CardDB& db) {
	if(!cons.Any())
		return true;
	std::printf("\n--- contraintes de ligne ---\n");
	for(const auto& [n, allowed] : cons.summons) {
		std::printf("  invocation #%u parmi :", n);
		for(uint32_t c : allowed)
			std::printf(" %s;", db.Name(c).c_str());
		std::printf("\n");
	}
	if(!cons.guard.empty()) {
		std::printf("  garde des l'invocation #%u, aux fenetres adverses :\n",
					cons.guard_after);
		for(size_t i = 0; i < cons.guard.size(); ++i) {
			std::printf("    %s", i ? "OU  " : "    ");
			for(size_t j = 0; j < cons.guard[i].size(); ++j)
				std::printf("%s%s", j ? " + " : "",
							db.Name(cons.guard[i][j].code).c_str());
			std::printf("\n");
		}
		if(cons.guard_opp_hand_release >= 0)
			std::printf("    eteinte quand la main adverse <= %d carte(s) "
						"(handrip)\n", cons.guard_opp_hand_release);
	}
	for(const auto& [code, zones] : cons.no_activate)
		std::printf("  activation interdite : %s (masque zones 0x%x)\n",
					db.Name(code).c_str(), zones);
	for(const auto& rq : cons.resolve_min)
		std::printf("  resolutions exigees  : %s x%u%s%s\n",
					db.Name(rq.code).c_str(), rq.min_count,
					rq.zones ? ", activee depuis " : "",
					rq.zones ? ZoneMaskName(rq.zones).c_str() : "");
	for(const auto& [code, mask] : cons.material_req)
		std::printf("  materiau exige       : %s invoque avec >=1 attribut 0x%x\n",
					db.Name(code).c_str(), mask);

	std::printf("\n  sequence de la reference (%zu invocations jusqu'au board) :\n",
				ref.summons_at_target);
	for(size_t i = 0; i < ref.summons_at_target && i < ref.summon_codes.size(); ++i)
		std::printf("      #%-3zu %s\n", i + 1,
					db.Name(db.Canonical(ref.summon_codes[i])).c_str());
	bool ok = true;
	for(const auto& [n, allowed] : cons.summons) {
		if(n > ref.summons_at_target)
			continue;   // semantique conditionnelle : pas de n-ieme, pas de faute
		uint32_t canon = db.Canonical(ref.summon_codes[n - 1]);
		if(std::find(allowed.begin(), allowed.end(), canon) == allowed.end()) {
			std::printf("\n  la reference VIOLE --summon #%u : son invocation "
						"#%u etait %s\n", n, n, db.Name(canon).c_str());
			ok = false;
		}
	}
	if(ref.guard_violations) {
		std::printf("\n  la reference VIOLE la garde : %zu fenetre(s) adverse(s) "
					"decouverte(s) sur %zu sous menace\n     (la premiere apres "
					"l'invocation #%zu, main adverse : %u carte(s))\n",
					ref.guard_violations, ref.guard_checks,
					ref.first_guard_violation_summon,
					ref.first_guard_violation_opp_hand);
		ok = false;
	} else if(!cons.guard.empty()) {
		std::printf("\n  garde : %zu fenetre(s) adverse(s) sous menace verifiee(s) "
					"sur la reference, toutes couvertes.\n", ref.guard_checks);
	}
	if(ref.forbidden_activations) {
		std::printf("  la reference UTILISE une activation interdite (%zu fois)\n",
					ref.forbidden_activations);
		ok = false;
	}
	for(size_t i = 0; i < cons.resolve_min.size(); ++i) {
		size_t have = i < ref.resolve_counts.size() ? ref.resolve_counts[i] : 0;
		if(have < cons.resolve_min[i].min_count) {
			std::printf("  la reference NE RESOUT PAS assez %s : %zu/%u\n",
						db.Name(cons.resolve_min[i].code).c_str(), have,
						cons.resolve_min[i].min_count);
			ok = false;
		} else {
			std::printf("  resolutions %s : %zu/%u sur la reference\n",
						db.Name(cons.resolve_min[i].code).c_str(), have,
						cons.resolve_min[i].min_count);
		}
	}
	if(ok)
		std::printf("\n  la reference satisfait les contraintes.\n");
	else
		std::printf("  => a zero ecart la recherche ne peut PAS retrouver la "
					"reference : 0 solution\n     y sera un resultat attendu, "
					"pas un defaut du moteur.\n");
	return ok;
}

// Une recherche ne vaut que ce que vaut son enumerateur : s'il ne sait pas
// proposer les choix qu'un joueur a reellement faits, il explore un autre jeu.
// On rejoue la ligne de reference et on verifie, a chaque decision, que la
// reponse enregistree figure bien parmi les reponses enumerees.
int RunEnumeratorCheck(Duel& duel, const Replay& yrp, const Options& opt,
					   Arena& arena) {
	std::printf("\n=== couverture de l'enumerateur ===\n");
	while(arena.Depth() > 1)
		arena.Pop();
	if(arena.Depth() == 0)
		arena.Push();
	arena.Restore();

	EnumOptions eo;
	eo.dedup_by_code = true;
	eo.max_subsets = opt.max_subsets;

	// Comparer les octets serait trop strict : EDOPro encode ses selections en
	// bitset (type 3), l'enumerateur en liste d'index (type 2), et la
	// deduplication par code choisit un representant qui n'est pas forcement
	// celui qu'a designe le joueur. Le seul critere qui a du sens est l'ETAT
	// ATTEINT : une reponse enumeree couvre la reponse enregistree si elle mene
	// exactement au meme etat. L'arene rend ce test abordable.
	// Rend l'empreinte EXACTE de l'etat, et au passage le hachage du BOARD au
	// sens du critere d'equivalence de la recherche. Les deux, parce qu'ils
	// separent trois causes que le message d'ecart confondait (session 17) :
	//   want == 0            : la reponse ENREGISTREE est rejetee ici — defaut
	//                          du harnais, pas de l'enumerateur ;
	//   board egal, etat non : une proposition realise la MEME INTENTION mais
	//                          `Fingerprint` les separe (la deduplication par
	//                          code choisit un representant que le joueur n'a
	//                          pas designe, et deux exemplaires identiques
	//                          n'occupent pas la meme sequence). C'est un ECART
	//                          DE REPRESENTATION, pas un trou d'espace ;
	//   ni l'un ni l'autre   : le coup est REELLEMENT absent de l'espace.
	// Sans cette separation, un artefact de deduplication se lit « la solution
	// est hors d'atteinte » — la famille de piege que ce dossier catalogue.
	uint64_t last_board = 0;
	auto advance_one = [&](const std::vector<uint8_t>& resp) -> uint64_t {
		last_board = 0;
		duel.SetResponse(resp);
		for(;;) {
			int st = duel.Process();
			bool retry = false;
			for(const Message& m : duel.Messages())
				if(m.type == MSG_RETRY)
					retry = true;
			if(retry)
				return 0;   // reponse rejetee par le core
			if(st != OCG_DUEL_STATUS_CONTINUE)
				break;
		}
		last_board = ComputeBoardKey(
						 duel, static_cast<uint8_t>(opt.target_player)).hash;
		return Fingerprint(duel);
	};

	// `by_board` : ecarts ou une proposition realise la MEME INTENTION (meme
	// board au sens du critere d'equivalence) sans atteindre le meme etat exact.
	// Ceux-la ne retirent RIEN de l'espace de recherche — les compter a part est
	// ce qui separe un artefact d'un vrai trou.
	struct Cov { int total = 0, covered = 0, empty = 0, by_board = 0; };
	std::map<uint8_t, Cov> cov;
	size_t ri = 0;
	uint8_t ptype = 0;
	std::vector<uint8_t> payload;
	int player = -1;
	std::vector<std::string> misses;
	std::vector<uint64_t> digests;

	for(;;) {
		int status = duel.Process();
		for(const Message& m : duel.Messages()) {
			if(IsPrompt(m.type)) {
				ptype = m.type;
				payload.assign(m.data, m.data + m.size);
				player = m.size ? m.data[0] : -1;
			}
		}
		if(status == OCG_DUEL_STATUS_END)
			break;
		if(status != OCG_DUEL_STATUS_AWAITING)
			continue;   // CONTINUE : le core a encore du travail
		if(ri >= yrp.responses.size())
			break;

		const auto recorded = yrp.responses[ri];
		auto choices = Enumerate(ptype, payload.data(),
								 static_cast<uint32_t>(payload.size()), eo);
		Cov& c = cov[ptype];
		++c.total;
		if(choices.empty())
			++c.empty;

		arena.Push();
		uint64_t want = advance_one(recorded);
		const uint64_t want_board = last_board;
		arena.Restore();
		bool found = false, same_board = false;
		for(const auto& ch : choices) {
			const uint64_t got = advance_one(ch.response);
			if(got == want && want != 0) {
				found = true;
				arena.Restore();
				break;
			}
			// MEME BOARD, etat different : l'intention est couverte, la
			// representation ne l'est pas.
			if(got && want && last_board == want_board)
				same_board = true;
			arena.Restore();
		}
		arena.Pop();

		if(found) {
			++c.covered;
		} else {
			if(same_board)
				++c.by_board;
			if(misses.size() < 8) {
				std::string why;
				if(!want)
					why = " — LA REPONSE ENREGISTREE EST REJETEE ICI (defaut du "
						  "harnais, pas de l'enumerateur)";
				else if(same_board)
					why = " — mais une proposition atteint le MEME BOARD : ecart "
						  "de REPRESENTATION (deduplication par code), pas un "
						  "trou d'espace";
				else
					why = " — et aucune n'atteint meme le meme BOARD : le coup "
						  "est REELLEMENT absent de l'espace";
				misses.push_back(std::string(PromptName(ptype)) + " #" +
								 std::to_string(ri) + " joueur " +
								 std::to_string(player) + " : aucune des " +
								 std::to_string(choices.size()) +
								 " propositions n'atteint l'etat enregistre" + why);
			}
		}
		// Les 290 etats de la ligne sont deux a deux distincts par construction
		// (le board change a chaque action). Si le digest en fusionne, il
		// coupera la branche du combo sans rien signaler.
		digests.push_back(StateDigest(duel, ptype, payload));

		duel.SetResponse(recorded);
		++ri;
	}
	arena.Restore();

	{
		std::map<uint64_t, size_t> first_seen;
		size_t collisions = 0;
		size_t first_at = 0, first_with = 0;
		for(size_t i = 0; i < digests.size(); ++i) {
			auto [it, fresh] = first_seen.emplace(digests[i], i);
			if(!fresh) {
				if(!collisions) { first_at = i; first_with = it->second; }
				++collisions;
			}
		}
		std::printf("  digest : %zu etats sur la ligne, %zu distincts, "
					"%zu fusions\n", digests.size(), first_seen.size(), collisions);
		if(collisions)
			std::printf("    premiere fusion : decision #%zu confondue avec #%zu"
						"  <-- le digest sous-hache, la recherche perdra des "
						"solutions\n", first_at, first_with);
	}

	int total = 0, covered = 0, by_board = 0;
	std::printf("  %-24s %8s %8s %8s %10s\n", "type", "n", "couvert", "taux",
				"m.board");
	for(const auto& [type, c] : cov) {
		std::printf("  %-24s %8d %8d %7.0f%% %10d%s\n", PromptName(type), c.total,
					c.covered, c.total ? 100.0 * c.covered / c.total : 0.0,
					c.by_board, c.empty ? "   (enumeration vide)" : "");
		total += c.total;
		covered += c.covered;
		by_board += c.by_board;
	}
	std::printf("  %-24s %8d %8d %7.0f%% %10d\n", "TOTAL", total, covered,
				total ? 100.0 * covered / total : 0.0, by_board);
	// LA COUVERTURE EFFECTIVE, celle qui compte pour la recherche : un ecart ou
	// une proposition realise la meme INTENTION ne retire rien de l'espace.
	if(by_board)
		std::printf("  %-24s %8d %8d %7.0f%%   <-- couverture EFFECTIVE "
					"(intentions), les %d ecart(s) « m.board » sont de "
					"REPRESENTATION\n", "TOTAL (au board)", total,
					covered + by_board,
					total ? 100.0 * (covered + by_board) / total : 0.0, by_board);
	if(!misses.empty()) {
		std::printf("\n  premiers ecarts :\n");
		for(const auto& s : misses)
			std::printf("    %s\n", s.c_str());
	}
	if(total == 0) {
		std::printf("\n  => AUCUNE decision observee : la verification n'a rien "
					"teste (bug du harnais)\n");
		return 1;
	}
	std::printf("\n  => %s\n", covered == total
		? "l'enumerateur reproduit integralement la ligne de reference"
		: "L'ENUMERATEUR NE COUVRE PAS LA LIGNE : toute recherche explore un "
		  "espace incomplet");
	return covered == total ? 0 : 1;
}

// Recherche guidee : atteindre le board cible, puis le faire mieux que la
// ligne de reference. Rend le nombre de solutions trouvees.
size_t RunSolve(Duel& duel, const Replay& yrp, const Options& opt, Arena& arena,
				const LineResult& ref, CardDB& db, ScriptProvider& scripts,
				uint32_t patience, const LineConstraints& cons) {
	std::printf("\n=== recherche guidee vers le board cible ===\n");
	while(arena.Depth() > 1)
		arena.Pop();
	if(arena.Depth() == 0)
		arena.Push();
	arena.Restore();

	BoardKey target;
	{
		size_t at = 0;
		at += Advance(duel, yrp, at, ref.target_at);
		target = ComputeBoardKey(duel, static_cast<uint8_t>(opt.target_player));
		arena.Restore();
	}
	// Controle preliminaire non negociable : la ligne de reference EST une
	// solution. Si le test de but ne se declenche pas en la rejouant, le defaut
	// est dans le test, pas dans la strategie de recherche — et tout resultat
	// de recherche serait sans valeur.
	uint32_t ref_actions = 0, ref_burned = 0;
	size_t ref_decisions = 0;
	{
		arena.Restore();
		size_t at = 0, hit_at = 0;
		bool hit = false;
		uint32_t best = 0;
		uint32_t acts = 0;
		while(at < yrp.responses.size()) {
			at += Advance(duel, yrp, at, 1, &acts);
			BoardKey k = ComputeBoardKey(duel, static_cast<uint8_t>(opt.target_player));
			uint32_t common = 0, i = 0, j = 0;
			while(i < k.codes.size() && j < target.codes.size()) {
				if(k.codes[i] == target.codes[j]) { ++common; ++i; ++j; }
				else if(k.codes[i] < target.codes[j]) ++i;
				else ++j;
			}
			best = (std::max)(best, common);
			if(k == target) {
				hit = true;
				hit_at = at;
				// Le cout de la reference doit etre mesure A L'INSTANT ou le
				// board est atteint, pas a la fin de l'enregistrement : le
				// replay continue apres, et comparer aux totaux ferait passer
				// pour un progres ce qui n'est que la fin du tour.
				auto con = static_cast<uint8_t>(opt.target_player);
				ref_actions = acts;
				ref_decisions = at;
				ref_burned = duel.Count(con, LOCATION_GRAVE) +
							 duel.Count(con, LOCATION_REMOVED);
				break;
			}
		}
		arena.Restore();
		std::printf("  controle       : le test de but %s en rejouant la "
					"reference%s\n",
					hit ? "SE DECLENCHE" : "NE SE DECLENCHE PAS",
					hit ? "" : "  <-- defaut du test de but, pas de la recherche");
		if(hit)
			std::printf("                   atteint a la decision #%zu\n", hit_at);
		else
			std::printf("                   au mieux %u des %zu cartes cibles "
						"reunies\n", best, target.codes.size());
		if(!hit)
			return 0;
	}

	std::printf("  cible          : %zu cartes\n", target.entries.size());
	std::printf("  reference      : %u actions, %zu decisions, %u cartes brulees"
				"  (mesure a l'instant du board)\n",
				ref_actions, ref_decisions, ref_burned);

	SearchConfig cfg;
	ApplyMechanisms(opt, cfg);   // chantier D : UN SEUL POINT DE CABLAGE
	// Bornes issues de la ligne de reference : on ne cherche que des lignes qui
	// ne sont pires ni en actions ni en decisions (section 3.2).
	cfg.max_decisions = static_cast<uint32_t>(ref_decisions);
	cfg.max_actions = ref_actions;
	cfg.time_limit_ms = opt.solve_ms;
	cfg.max_nodes = 50000000;
	cfg.max_solutions = 16;
	// Optimisation anytime : une passe ne s'arrete plus a 16 solutions a cout
	// egal — elle EPUISE l'espace des k deviations (c'est ce qui rend la
	// « resistance de la reference » une preuve, pas un arret premature :
	// mesure, chaque passe s'arretait a 16 variantes en 0,2 s / 330 etats).
	// Les bornes actions/decisions se relachent : une ligne qui RECUPERE des
	// brulees paie des actions en plus — les bornes <= reference interdiraient
	// exactement les lignes moins cheres en tier 1.
	if(opt.optimize) {
		cfg.anytime = true;
		cfg.max_solutions = 24;
		cfg.max_actions = ref_actions + 8;
		cfg.max_decisions = static_cast<uint32_t>(ref_decisions) + 48;
		std::printf("  OPTIMISATION anytime : bornes relachees a %u actions / "
					"%u decisions,\n  epuisement des passes sous borne de cout.\n",
					cfg.max_actions, cfg.max_decisions);
	}
	cfg.enumeration.dedup_by_code = true;
	cfg.enumeration.max_subsets = opt.max_subsets;
	// Deux drapeaux d'enumeration qui existaient sans cadran (repares s15) :
	// les sorties de phase (lues au seul prompt idle jusqu'ici) et les zones
	// canoniques (declarees, allumees nulle part).
	cfg.enumeration.canonical_zones = opt.canonical_zones;
	// Cible POSEE -> inclusion par defaut ; cible CAPTUREE -> egalite exacte.
	cfg.goal_subset = opt.target_subset ||
					  (!opt.target_specs.empty() && !opt.target_exact);
	cfg.summon_constraints = cons.summons;
	cfg.guard_after = cons.guard_after;
	cfg.guard_clauses = cons.guard;
	cfg.guard_opp_hand_release = cons.guard_opp_hand_release;
	cfg.resolve_min = cons.resolve_min;
	CheckSaturations(target.codes.size(), cons.resolve_min);
	cfg.material_req = cons.material_req;
	cfg.hint_cards = cons.hints;
	if(!cons.no_activate.empty()) {
		cfg.enumeration.no_activate = &cons.no_activate;
		// Le filtre compare des codes canoniques : il faut la table des alias.
		cfg.enumeration.db = &db;
	}
	if(!cons.no_chain.empty())
		cfg.enumeration.no_chain = &cons.no_chain;
	if(!cons.self_negate.empty())
		cfg.self_negate = &cons.self_negate;   // s22ter : discipline choisie
	cfg.enumeration.mp1_only = opt.mp1_only;   // s22quater : combo en MP1 seule
	const bool ref_meets_cons = ReportConstraints(cons, ref, db);

	// Ligne de reference relevee une fois pour tous les workers : digests
	// (resynchronisation exacte — un etat qui EST un point plus loin de la
	// ligne y reprend le suffixe enregistre) et plan_keys par index (repertoire
	// FENETRE — apres une deviation, rejouer un coup voisin de la reference est
	// gratuit). Sans les deux, echanger les invocations #4/#5 etait introuvable
	// jusqu'a k=12 (mesure).
	std::unordered_map<uint64_t, size_t> ref_digests;
	std::vector<uint64_t> ref_keys;
	{
		auto t0 = Clock::now();
		// Le releve REJOUE la reference, qui FINIT SON TOUR : lui retirer les
		// sorties de phase rendrait sa derniere reponse non enumerable et
		// trouerait le repertoire en silence. --no-phase-change borne la
		// RECHERCHE, pas la lecture de ce qui a ete joue.
		EnumOptions leo = cfg.enumeration;
		RefLineStats rls;
		LiftRefLine(duel, arena, yrp, opt.target_player, ref_decisions,
					leo, ref_digests, ref_keys, &rls);
		arena.Restore();
		size_t known = 0;
		for(uint64_t k : ref_keys)
			known += k != 0;
		std::printf("  ligne relevee  : %zu digests, %zu/%zu coups identifies "
					"(%.0f ms)\n", ref_digests.size(), known, ref_keys.size(),
					MsSince(t0));

		// LA VRAISEMBLANCE DE LA REFERENCE, ET C'EST LE SEUL CHIFFRE QUI DISE SI
		// L'ECHANTILLONNAGE A UNE CHANCE.
		//
		// A politique NEUVE, tous les poids `plan_key` partent a ZERO et aucun
		// biais n'est arme : les logits de `search.cpp` sont donc tous egaux et
		// le softmax est UNIFORME. La probabilite qu'un tirage reproduise la
		// ligne vaut exactement le produit des inverses d'arites — un calcul,
		// pas une estimation. 9.28 en donnait une approximation (`0,66^32`) sur
		// les seules decisions idle d'un plan partiel ; ici c'est exact et sur
		// la ligne entiere.
		//
		// Et la COUVERTURE se lit enfin sans confusion : « retrouve » (le coup
		// est dans l'espace d'actions) n'est pas « identifie » (il est au
		// repertoire). Un seul coup NON RETROUVE rend la ligne inatteignable a
		// tout budget, et aucun compteur ne le disait.
		{
			size_t own = 0, forced = 0, branchy = 0, found = 0, absent = 0;
			double log10p = 0.0;
			uint32_t worst = 0;
			size_t worst_at = 0;
			for(size_t i = 0; i < rls.arity.size(); ++i) {
				const uint32_t a = rls.arity[i];
				if(!a)
					continue;   // decision adverse : non enumeree
				++own;
				if(a == 1)
					++forced;
				else {
					++branchy;
					log10p -= std::log10(static_cast<double>(a));
					if(a > worst) { worst = a; worst_at = i; }
				}
				if(rls.matched[i]) ++found; else ++absent;
			}
			std::printf("\n  --- VRAISEMBLANCE DE LA REFERENCE (politique "
						"NEUVE = softmax uniforme) ---\n");
			std::printf("  decisions du joueur : %zu  (forcees %zu, a choix "
						"%zu)\n", own, forced, branchy);
			std::printf("  COUVERTURE : coup de la reference RETROUVE dans "
						"l'enumeration %zu/%zu", found, own);
			if(absent)
				std::printf("   <<< %zu ABSENT(S) : la ligne est HORS de "
							"l'espace d'actions", absent);
			std::printf("\n");
			// QUEL coup manque, et a quelle arite. Un trou de couverture qu'on
			// ne localise pas ne se repare pas — et un seul suffit a rendre la
			// ligne inatteignable a tout budget.
			for(const RefLineStats::Miss& m : rls.misses) {
				std::printf("     decision #%zu  %s : %zu choix enumere(s), "
							"AUCUN ne reproduit la reference\n", m.index,
							PromptName(m.prompt), m.offered.size());
				auto hex = [](const std::vector<uint8_t>& v) {
					std::string s;
					char b[8];
					for(uint8_t x : v) {
						std::snprintf(b, sizeof b, "%02x ", x);
						s += b;
					}
					return s;
				};
				std::printf("        REELLE  : %s\n", hex(m.recorded).c_str());
				for(size_t j = 0; j < m.offered.size(); ++j)
					std::printf("        offerte : %s\n",
								hex(m.offered[j]).c_str());
			}
			std::printf("  arite max : %u (decision #%zu)   |   arite moyenne "
						"geometrique : %.2f\n", worst, worst_at,
						branchy ? std::pow(10.0, -log10p / double(branchy))
								: 1.0);
			std::printf("  log10 P(tirage uniforme reproduit la ligne) = "
						"%.1f   =>  UNE CHANCE SUR 10^%.0f\n", log10p, -log10p);
			// OU PART L'IMPROBABILITE ? Un seul nombre ne le dit pas, et la
			// reponse departage deux chantiers opposes : si l'essentiel vient
			// de decisions qui NE PEUVENT PAS affecter le but (position, zone,
			// ordre de materiaux equivalents), un QUOTIENT mecanique suffit ;
			// si tout est dans IDLECMD et les selections, seule une
			// SERIALISATION en sous-buts peut aider. Le calcul est gratuit :
			// l'arite de chaque decision est deja relevee.
			{
				struct Agg { double log10p = 0.0; size_t n = 0; uint32_t mx = 0; };
				std::map<uint8_t, Agg> by;
				for(size_t i = 0; i < rls.arity.size(); ++i) {
					const uint32_t a = rls.arity[i];
					if(a < 2)
						continue;
					Agg& g = by[rls.prompt[i]];
					g.log10p += std::log10(static_cast<double>(a));
					++g.n;
					g.mx = (std::max)(g.mx, a);
				}
				std::vector<std::pair<double, uint8_t>> ord;
				for(const auto& [t, g] : by)
					ord.emplace_back(g.log10p, t);
				std::sort(ord.rbegin(), ord.rend());
				std::printf("  --- d'ou viennent les %.0f ordres de grandeur "
							"---\n", -log10p);
				for(const auto& [v, t] : ord) {
					const Agg& g = by[t];
					std::printf("   %-24s %6.1f  (%zu decision(s), arite max "
								"%u, moy. geo. %.2f)\n", PromptName(t), v, g.n,
								g.mx, std::pow(10.0, v / double(g.n)));
				}
			}
			std::printf("  Lecture : c'est la borne SANS apprentissage. La "
						"politique ne peut la remonter\n"
						"  que sur les decisions qu'elle revoit — une ligne "
						"jamais echantillonnee\n"
						"  n'adapte rien, et c'est la l'oeuf et la poule du "
						"dossier, enfin chiffre.\n");
		}
	}
	cfg.ref_digests = &ref_digests;
	// Le repertoire fenetre ne sert que la reparation SOUS contraintes, son cas
	// d'usage : sans contrainte, il depense le budget en permutations de la
	// reference (cout egal par construction) et divise par deux la profondeur
	// k atteinte a budget fixe (mesure : k=4 contre k=8 a 90 s).
	if(cons.Any())
		cfg.ref_keys = &ref_keys;

	// Approfondissement progressif du nombre d'ecarts. A zero ecart la
	// recherche rejoue la reference, donc elle trouve toujours au moins une
	// solution : "aucune solution" redevient un signal de defaut, pas un
	// resultat possible.
	std::vector<Solution> sols;
	double spent = 0;
	unsigned threads = opt.threads ? opt.threads
								   : (std::max)(1u, std::thread::hardware_concurrency());
	std::printf("  workers        : %u\n", threads);
	std::printf("  nouveaute      : %s (patience %u)\n",
				patience ? "active" : "desactivee", patience);
	// Le mode `--solve` est le CONTROLE DE SANTE, et il n'a jamais vu un seul
	// mecanisme jusqu'ici. Il les recoit desormais — et il DIT lesquels, ce qui
	// est la seule facon de savoir si une sante a ete mesuree nue ou non.
	ReportMechanisms(cfg, "solve");

	struct PassOut {
		std::vector<Solution> found;
		uint64_t nodes = 0, transpos = 0, cuts = 0, resyncs = 0;
		uint64_t goal_hits = 0;   // re-atteintes d'apres-but comprises
		CutCounts cut;
		bool timed_out = false;
		double ms = 0;
	};
	// Une passe a k ecarts, avec ou sans elagage par nouveaute. Factorise pour
	// que le controle A/B compare EXACTEMENT le meme moteur.
	auto run_pass = [&](uint32_t k, uint32_t pat, double budget) {
		PassOut out;
		auto t0 = Clock::now();
		// k = 0 suit un chemin unique : rien a paralleliser, et c'est le
		// controle qui doit retrouver la reference.
		unsigned n = (k == 0) ? 1u : threads;
		// Un jeton par point de deviation possible le long de l'echine. La cle
		// est l'indice de reference, borne par la profondeur : la table est
		// dimensionnee a 4x, elle ne peut pas saturer ici (contrairement a la
		// passe de transplantation, ou la cle est un digest d'etat).
		ClaimTable claims(ref_decisions + 1);
		// Table de transposition PARTAGEE de la passe (lazy SMP) : un etat
		// resolu par un worker elague chez tous — les tables privees
		// refaisaient le meme travail. Fraiche par passe, comme l'etaient les
		// tables privees.
		std::unique_ptr<SharedTT> stt;
		if(opt.tt_mb && n > 1)
			stt = std::make_unique<SharedTT>(opt.tt_mb);
		std::mutex merge;

		auto worker = [&](unsigned) {
			// Chaque worker a SA propre arene et SON propre duel : les
			// instantanes ne circulent pas entre threads (bases distinctes).
			Arena local_arena;
			std::string err;
			if(!local_arena.Init(opt.arena_mb << 20, 0, err))
				return WorkerAbort("arene (reparation)", err);
			// Le duel vit DANS l'arene : il doit etre detruit avant elle,
			// sinon OCG_DestroyDuel travaille sur de la memoire rendue a l'OS.
			{
				Duel local(db, scripts, &local_arena);
				if(local.Create(yrp.seed, yrp.duel_flags, yrp.start_lp,
								yrp.start_hand, yrp.draw_count, err) &&
				   local.Setup(yrp, err)) {
					if(opt.stop_gc)
						local.SetLuaGc(false);
					SearchConfig wcfg = cfg;
					wcfg.time_limit_ms = budget;
					wcfg.novelty_patience = pat;
					wcfg.shared_tt = stt.get();
					if(n > 1) {
						// A un seul ecart il n'y a pas de second niveau : on
						// partage le premier, faute de mieux.
						wcfg.claim_level = (k <= 1) ? 0u : 1u;
						wcfg.claims = &claims;
					}
					Search s(local, local_arena, yrp, wcfg);
					s.RunRepair(target, k);
					std::lock_guard<std::mutex> lock(merge);
					for(const auto& x : s.Solutions())
						out.found.push_back(x);
					out.nodes += s.Stats().nodes;
					out.transpos += s.Stats().transpositions;
					out.cuts += s.Stats().novelty_cuts;
					out.resyncs += s.Stats().resyncs;
					out.goal_hits += s.Stats().goal_hits;
					out.cut.Add(s.Stats());
					out.timed_out |= s.Stats().hit_time_limit;
				} else {
					WorkerAbort("duel (reparation)", err);
				}
			}
			ReportPoison("reparation", local_arena);
			local_arena.Shutdown();
		};

		// Aucun worker sur le thread principal : son arene y est deja
		// proprietaire, et un second Init lui volerait le routage des
		// liberations (les objets du duel principal partiraient vers free()).
		std::vector<std::thread> pool;
		for(unsigned i = 0; i < n; ++i)
			pool.emplace_back(worker, i);
		for(auto& t : pool)
			t.join();
		out.ms = MsSince(t0);
		if(prof::enabled) {
			char lbl[32];
			std::snprintf(lbl, sizeof(lbl), "ecarts k=%u", k);
			prof::PrintPhase(lbl);
		}
		return out;
	};

	// Meilleure solution au sens lexicographique retenu.
	auto best_of = [](const std::vector<Solution>& v) -> const Solution* {
		const Solution* best = nullptr;
		for(const auto& s : v) {
			if(!best ||
			   std::tie(s.burned, s.actions, s.decisions) <
				   std::tie(best->burned, best->actions, best->decisions))
				best = &s;
		}
		return best;
	};

	std::printf("\n  %-8s %10s %12s %11s %10s %9s\n", "ecarts", "solutions",
				"etats", "transpos.", "coupures", "duree");
	uint32_t reached = 0;
	{
		PassOut o = run_pass(0, 0, opt.solve_ms - spent);
		spent += o.ms;
		std::printf("  %-8u %10zu %12llu %11llu %10llu %8.1f s%s\n", 0u,
					o.found.size(), (unsigned long long)o.nodes,
					(unsigned long long)o.transpos, (unsigned long long)o.cuts,
					o.ms / 1000.0, o.timed_out ? "  (budget epuise)" : "");
		PrintCuts(o.cut);
		for(const auto& x : o.found)
			sols.push_back(x);
		if(o.found.empty()) {
			if(!ref_meets_cons) {
				std::printf("       (attendu : la reference viole une contrainte "
							"de ligne, elle ne peut pas etre retrouvee)\n");
			} else {
				std::printf("\n  A zero ecart la reference doit etre retrouvee. "
							"Elle ne l'est pas : defaut du moteur.\n");
				return 0;
			}
		}
	}

	uint32_t k_start = 1;
	if(patience && spent < opt.solve_ms) {
		// CONTROLE A/B — la discipline de verification l'exige : un elagage qui
		// gagne 10x en etats mais perd des solutions doit LE DIRE LUI-MEME.
		// Meme moteur, meme budget, k = 1, avec puis sans nouveaute.
		reached = 1;
		double slice = (std::min)((opt.solve_ms - spent) / 4.0, 20000.0);
		PassOut a = run_pass(1, 0, slice);
		spent += a.ms;
		PassOut b = run_pass(1, patience, slice);
		spent += b.ms;
		const Solution* ba = best_of(a.found);
		const Solution* bb = best_of(b.found);
		std::printf("\n--- controle A/B de la nouveaute (k=1, %.0f s chacun) ---\n",
					slice / 1000.0);
		std::printf("  sans : %10llu etats  %5zu solutions\n",
					(unsigned long long)a.nodes, a.found.size());
		std::printf("  avec : %10llu etats  %5zu solutions  %llu coupures  "
					"(etats %+.0f%%)\n",
					(unsigned long long)b.nodes, b.found.size(),
					(unsigned long long)b.cuts,
					a.nodes ? 100.0 * (double(b.nodes) - double(a.nodes)) /
								  double(a.nodes) : 0.0);
		bool lost_best = ba && (!bb ||
			std::tie(bb->burned, bb->actions, bb->decisions) >
				std::tie(ba->burned, ba->actions, ba->decisions));
		if(ba && bb)
			std::printf("  meilleur cout : sans (%u,%u,%u)  avec (%u,%u,%u)  => %s\n",
						ba->burned, ba->actions, ba->decisions,
						bb->burned, bb->actions, bb->decisions,
						lost_best ? "la nouveaute PERD le meilleur cout"
								  : "meilleur cout conserve");
		else if(ba && !bb)
			std::printf("  meilleur cout : sans (%u,%u,%u)  avec AUCUN  "
						"=> la nouveaute PERD des solutions\n",
						ba->burned, ba->actions, ba->decisions);
		for(const auto& x : a.found)
			sols.push_back(x);
		for(const auto& x : b.found)
			sols.push_back(x);
		k_start = 2;
	}

	std::printf("\n  %-8s %10s %12s %11s %10s %9s\n", "ecarts", "solutions",
				"etats", "transpos.", "coupures", "duree");
	for(uint32_t k = k_start; k <= 12 && spent < opt.solve_ms; ++k) {
		reached = k;
		PassOut o = run_pass(k, patience, opt.solve_ms - spent);
		spent += o.ms;
		char resync[48] = "";
		if(o.resyncs)
			std::snprintf(resync, sizeof(resync), "  %llu resync",
						  (unsigned long long)o.resyncs);
		// Session 6 : les atteintes du but (re-atteintes comprises sous
		// --optimize, piege 35) ne se lisaient nulle part.
		char goals[40] = "";
		if(opt.optimize && o.goal_hits)
			std::snprintf(goals, sizeof(goals), "  %llu atteinte(s)",
						  (unsigned long long)o.goal_hits);
		std::printf("  %-8u %10zu %12llu %11llu %10llu %8.1f s%s%s%s\n", k,
					o.found.size(), (unsigned long long)o.nodes,
					(unsigned long long)o.transpos, (unsigned long long)o.cuts,
					o.ms / 1000.0, o.timed_out ? "  (budget epuise)" : "", resync,
					goals);
		PrintCuts(o.cut);
		for(const auto& x : o.found)
			sols.push_back(x);
	}
	arena.Restore();

	if(sols.empty()) {
		std::printf("\n  AUCUNE solution atteinte.\n");
		return 0;
	}

	std::printf("\n  %zu solution(s). Classement lexicographique : cartes brulees,\n"
				"  puis actions, puis decisions.\n\n", sols.size());
	std::sort(sols.begin(), sols.end(), [](const Solution& a, const Solution& b) {
		if(a.burned != b.burned) return a.burned < b.burned;
		if(a.actions != b.actions) return a.actions < b.actions;
		return a.decisions < b.decisions;
	});
	std::printf("  %-4s %10s %9s %11s %8s %8s %8s\n", "#", "brulees", "actions",
				"decisions", "main", "deck", "extra");
	int better = 0;
	for(size_t i = 0; i < sols.size() && i < 10; ++i) {
		const Solution& x = sols[i];
		// Strictement meilleur au sens lexicographique retenu : moins de
		// cartes brulees, ou autant mais moins d'actions.
		bool wins = x.burned < ref_burned ||
					(x.burned == ref_burned && x.actions < ref_actions);
		if(wins)
			++better;
		std::printf("  %-4zu %10u %9u %11u %8u %8u %8u%s\n", i, x.burned, x.actions,
					x.decisions, x.hand_left, x.deck_left, x.extra_left,
					wins ? "   <-- meilleure que la reference" : "");
	}
	std::printf("\n  %-4s %10u %9u %11zu   (reference)\n", "ref", ref_burned,
				ref_actions, ref_decisions);
	WriteSolutions(sols, yrp, target, opt, db, scripts, opt.outdir, cons);
	if(!better)
		std::printf("\n  Aucune ligne strictement meilleure trouvee.\n"
					"  La reference resiste a %u deviation(s) simultanee(s).\n",
					reached);
	(void)ref;
	(void)db;
	return sols.size();
}

// Main d'ouverture d'un replay, lue sur un duel jetable monte pour l'occasion.
// C'est la seule facon de la connaitre : elle depend de la graine et du melange
// du core, pas du fichier.
std::vector<uint32_t> OpeningHand(const Replay& yrp, uint8_t con, CardDB& db,
								  ScriptProvider& scripts, size_t arena_mb) {
	std::vector<uint32_t> out;
	// Sur son propre thread, imperativement : Arena::Init s'approprie le
	// routage des liberations du thread courant (t_owner). Monter une seconde
	// arene sur le thread principal deposseder ait l'arene du duel de reference,
	// dont les objets partiraient ensuite vers free().
	std::thread([&] {
		Arena a;
		std::string err;
		if(!a.Init(arena_mb << 20, 0, err))
			return WorkerAbort("arene (sonde de main)", err);
		{
			Duel d(db, scripts, &a);
			if(d.Create(yrp.seed, yrp.duel_flags, yrp.start_lp, yrp.start_hand,
						yrp.draw_count, err) && d.Setup(yrp, err)) {
				while(d.Process() == OCG_DUEL_STATUS_CONTINUE) {}
				for(const auto& c : d.Query(con, LOCATION_HAND,
											QUERY_CODE | QUERY_ALIAS))
					if(c.present)
						out.push_back(db.Canonical(c.Code()));
			} else {
				WorkerAbort("duel (sonde de main)", err);
			}
		}
		a.Shutdown();
	}).join();
	return out;
}

// Charge une decklist .ydk : codes du main et de l'extra, side ignore.
bool LoadYdk(const std::string& path, Deck& out, std::string& error) {
	std::ifstream in(path);
	if(!in) {
		error = "decklist illisible : " + path;
		return false;
	}
	std::vector<uint32_t>* section = nullptr;
	std::string line;
	while(std::getline(in, line)) {
		line = Trimmed(line);
		if(line.empty())
			continue;
		if(line[0] == '#' || line[0] == '!') {
			if(line == "#main")       section = &out.main;
			else if(line == "#extra") section = &out.extra;
			else if(line[0] == '!')   section = nullptr;   // side : hors duel
			continue;
		}
		char* end = nullptr;
		unsigned long code = std::strtoul(line.c_str(), &end, 10);
		if(end && *end == '\0' && code > 0 && section)
			section->push_back(static_cast<uint32_t>(code));
	}
	if(out.main.empty()) {
		error = "aucune carte dans le main de " + path;
		return false;
	}
	return true;
}

// Copie les champs d'un Replay (la classe est non copiable a cause du yrp1
// embarque) — tout SAUF reponses, paquets et yrp embarque : c'est une position
// de depart, pas une ligne.
void CopyReplayHeader(const Replay& src, Replay& dst) {
	dst.id = src.id;
	dst.version = src.version;
	dst.flag = src.flag;
	dst.timestamp = src.timestamp;
	dst.header_version = src.header_version;
	std::memcpy(dst.seed, src.seed, sizeof(dst.seed));
	std::memcpy(dst.props, src.props, sizeof(dst.props));
	dst.start_lp = src.start_lp;
	dst.start_hand = src.start_hand;
	dst.draw_count = src.draw_count;
	dst.duel_flags = src.duel_flags;
	dst.scriptname = src.scriptname;
	dst.home_count = src.home_count;
	dst.opposing_count = src.opposing_count;
	dst.players = src.players;
	dst.decks = src.decks;
	dst.rule_cards = src.rule_cards;
}

// Construit une position de DEPART synthetique : le duel de la reference —
// memes parametres, meme adversaire — mais avec CE deck et CETTE main.
//
// La main est forcee par DUEL_PSEUDO_SHUFFLE (le core ne melange plus) plus le
// reordonnancement du main deck. L'extremite qui se pioche depend du core : on
// ne la devine pas, on la VERIFIE — un duel jetable pioche la main, et si elle
// ne correspond pas on essaie l'autre extremite. Un echec des deux cotes est
// une erreur franche, jamais une recherche sur une main qu'on croit avoir.
bool BuildSyntheticStart(const Replay& ref, const Deck& ydk,
						 const std::vector<uint32_t>& hand, CardDB& db,
						 ScriptProvider& scripts, size_t arena_mb,
						 Replay& out, std::string& error) {
	CopyReplayHeader(ref, out);
	out.duel_flags |= DUEL_PSEUDO_SHUFFLE;
	out.start_hand = static_cast<uint32_t>(hand.size());
	out.decks.resize(2);
	out.decks[1] = ref.decks.size() > 1 ? ref.decks[1] : Deck{};
	out.decks[0].extra = ydk.extra;

	// Retirer UNE occurrence de chaque carte de main du reste du deck,
	// par identite canonique (la decklist peut porter une autre illustration).
	std::vector<uint32_t> rest = ydk.main, hand_codes;
	for(uint32_t want : hand) {
		uint32_t canon = db.Canonical(want);
		bool found = false;
		for(size_t i = 0; i < rest.size(); ++i) {
			if(db.Canonical(rest[i]) == canon) {
				hand_codes.push_back(rest[i]);
				rest.erase(rest.begin() + i);
				found = true;
				break;
			}
		}
		if(!found) {
			error = "la main demandee contient " + db.Name(canon) +
					" qui n'est pas (assez) dans la decklist";
			return false;
		}
	}

	auto matches = [&](const std::vector<uint32_t>& got) {
		if(got.size() != hand.size())
			return false;
		std::vector<uint32_t> a = got, b;
		for(uint32_t c : hand)
			b.push_back(db.Canonical(c));
		std::sort(a.begin(), a.end());
		std::sort(b.begin(), b.end());
		return a == b;
	};

	// Essai 1 : la main a la FIN du main deck (le core pioche sur le dessus,
	// qui est la queue de la liste) ; essai 2 : au DEBUT.
	for(int attempt = 0; attempt < 2; ++attempt) {
		out.decks[0].main.clear();
		if(attempt == 0) {
			out.decks[0].main = rest;
			out.decks[0].main.insert(out.decks[0].main.end(),
									 hand_codes.begin(), hand_codes.end());
		} else {
			out.decks[0].main = hand_codes;
			out.decks[0].main.insert(out.decks[0].main.end(),
									 rest.begin(), rest.end());
		}
		auto got = OpeningHand(out, 0, db, scripts, arena_mb);
		if(matches(got))
			return true;
	}
	error = "impossible de forcer la main demandee : le core ne pioche ni la "
			"tete ni la queue du deck dans cet ordre (verifier les drapeaux "
			"du duel de reference)";
	return false;
}

// Ce que la recherche a su poser, en face de ce qu'il fallait. Un decompte
// ("5 des 8") ne se traduit en decision que si l'on sait LESQUELLES manquent.
void ReportBestBoard(const std::vector<uint32_t>& best, const BoardKey& target,
					 const CardDB& db) {
	if(best.empty())
		return;
	std::map<uint32_t, int> delta;
	for(uint32_t c : target.codes) ++delta[c];
	for(uint32_t c : best) --delta[c];
	std::printf("\n--- meilleur board atteint, face a la cible ---\n");
	for(uint32_t c : best)
		std::printf("      pose      %9u  %s\n", c, db.Name(c).c_str());
	for(const auto& [code, n] : delta)
		if(n > 0)
			std::printf("      MANQUE %dx %9u  %s\n", n, code,
						db.Name(code).c_str());
	for(const auto& [code, n] : delta)
		if(n < 0)
			std::printf("      en trop%3dx %9u  %s\n", -n, code,
						db.Name(code).c_str());
}

// Ecrit les solutions en replays rejouables, apres les avoir VERIFIEES.
//
// Une solution sort d'une recherche qui deduplique et canonicalise : rien ne
// garantit a priori que sa suite de reponses rejouee depuis zero refasse le
// board. On la rejoue donc dans un duel neuf et on n'ecrit que ce qui tient.
size_t WriteSolutions(const std::vector<Solution>& sols, const Replay& start_yrp,
					  const BoardKey& target, const Options& opt, CardDB& db,
					  ScriptProvider& scripts, const std::string& outdir,
					  const LineConstraints& cons,
					  const std::vector<uint32_t>* opp_hand,
					  const std::vector<BoardKey>* target_alts) {
	std::error_code ec;
	std::filesystem::create_directories(outdir, ec);
	// Plafond d'ecriture, nomme au lieu d'etre un 16 nu au fond d'une boucle.
	constexpr size_t kMaxWritten = 16;
	size_t written = 0, rejected = 0;
	size_t rej_retry = 0, rej_cons = 0, rej_board = 0;
	// Rejets pour contrainte --summon JAMAIS ATTEINTE (3.4) : distincts d'une
	// contrainte VIOLEE, et bien plus instructifs — ils disent que la ligne
	// s'arrete avant le point que l'experience vise.
	size_t rej_never = 0;
	const auto con = static_cast<uint8_t>(opt.target_player);
	// Sequence d'invocations de la premiere solution ecrite : c'est la preuve
	// visible qu'une contrainte --summon est tenue.
	std::vector<uint32_t> first_summons;

	// Thread dedie : une arene ne s'initialise jamais sur un thread qui en
	// possede deja une.
	std::thread([&] {
		Arena a;
		std::string err;
		if(!a.Init(opt.arena_mb << 20, 0, err))
			return WorkerAbort("arene (prior par rejeu)", err);
		{
			Duel d(db, scripts, &a);
			if(!d.Create(start_yrp.seed, start_yrp.duel_flags, start_yrp.start_lp,
						 start_yrp.start_hand, start_yrp.draw_count, err) ||
			   !d.Setup(start_yrp, err, opp_hand,
						static_cast<uint8_t>(1 - opt.target_player)))
				return WorkerAbort("duel (prior par rejeu)", err);
			if(opt.stop_gc)
				d.SetLuaGc(false);
			a.Push();
			// Plafond d'ecriture. Sous --optimize, max_solutions vaut 24 PAR
			// worker et les ensembles fusionnent : la troncature est la regle,
			// pas l'exception. Aux sites qui ne trient pas d'abord (--fire,
			// AR.sols), les seize retenues ne sont pas les moins cheres, ce sont
			// les seize ARRIVEES EN PREMIER — et `written`/`rejected` ne
			// permettaient pas de le voir, puisque sols.size() n'etait jamais
			// imprime (4.9).
			for(size_t i = 0; i < sols.size() && i < kMaxWritten; ++i) {
				size_t used = 0;
				bool retry = false;
				bool guard_ok = true;
				bool material_ok = true;
				int pplayer = -1;
				std::vector<uint32_t> summons, mats;
				std::vector<size_t> resolves(cons.resolve_min.size(), 0);
				auto scan = [&] {
					for(const Message& m : d.Messages()) {
						if(m.type == MSG_RETRY)
							retry = true;
						if(m.type == MSG_MOVE && !cons.material_req.empty() &&
						   m.size >= 28) {
							uint32_t c = 0, reason = 0;
							std::memcpy(&c, m.data, 4);
							std::memcpy(&reason, m.data + 24, 4);
							if((reason & REASON_SYNCHRO) &&
							   (reason & REASON_MATERIAL))
								mats.push_back(c);
						}
						if((m.type == MSG_SUMMONING || m.type == MSG_SPSUMMONING) &&
						   m.size >= 4) {
							uint32_t c = 0;
							std::memcpy(&c, m.data, 4);
							summons.push_back(c);
							if(!cons.resolve_min.empty() && c) {
								const uint32_t sc = db.Canonical(c);
								for(size_t k = 0; k < cons.resolve_min.size(); ++k)
									if(cons.resolve_min[k].on_summon &&
									   cons.resolve_min[k].code == sc)
										++resolves[k];
							}
							if(!cons.material_req.empty() && c) {
								uint32_t canon = db.Canonical(c);
								for(const auto& [card, attrs] : cons.material_req) {
									if(card != canon)
										continue;
									bool ok = false;
									for(uint32_t mc : mats) {
										const CardRow* row = db.Find(mc);
										if(row && (row->attribute & attrs)) {
											ok = true;
											break;
										}
									}
									if(!ok)
										material_ok = false;
								}
								mats.clear();
							}
						}
						if(m.type == MSG_CHAINING && !cons.resolve_min.empty() &&
						   m.size >= 4) {
							uint32_t c = 0;
							std::memcpy(&c, m.data, 4);
							c = db.Canonical(c);
							const uint32_t loc = ChainingLocation(m.data, m.size);
							for(size_t k = 0; k < cons.resolve_min.size(); ++k)
								if(!cons.resolve_min[k].on_summon &&
								   cons.resolve_min[k].code == c &&
								   (!cons.resolve_min[k].zones ||
									(loc & cons.resolve_min[k].zones)))
									++resolves[k];
						}
						if(IsPrompt(m.type))
							pplayer = m.size ? m.data[0] : -1;
					}
				};
				while(used < sols[i].responses.size() && !retry) {
					int status = d.Process();
					scan();
					if(status == OCG_DUEL_STATUS_AWAITING) {
						// Fenetre adverse sous garde : la re-verifier ici fait
						// partie du contrat "verifie avant ecriture".
						if(!cons.guard.empty() && guard_ok &&
						   pplayer == 1 - opt.target_player &&
						   summons.size() >= cons.guard_after &&
						   (cons.guard_opp_hand_release < 0 ||
							static_cast<int>(d.Count(
								static_cast<uint8_t>(1 - opt.target_player),
								LOCATION_HAND)) > cons.guard_opp_hand_release)) {
							BoardKey fk = ComputeBoardKey(d, con);
							if(!GuardHolds(d, con, cons.guard, fk.codes))
								guard_ok = false;
						}
						d.SetResponse(sols[i].responses[used++]);
					} else if(status != OCG_DUEL_STATUS_CONTINUE)
						break;
				}
				while(d.Process() == OCG_DUEL_STATUS_CONTINUE)
					scan();
				// La recherche a deja impose les contraintes le long du chemin ;
				// on re-verifie ici parce que "verifie avant ecriture" ne
				// souffre pas d'exception.
				bool cons_ok = guard_ok && material_ok;
				size_t summon_never = 0;
				for(const auto& [n, allowed] : cons.summons) {
					// La n-ieme invocation n'a JAMAIS eu lieu. Cote recherche
					// c'est une contrainte de PREFIXE, donc conditionnelle par
					// construction. Ici on est dans le controle « verifie avant
					// ecriture, qui ne souffre pas d'exception » : laisser passer
					// revenait a ecrire comme CONFORME une ligne sur laquelle la
					// contrainte autour de laquelle l'experience est batie n'a
					// jamais ete exercee, puis a la compter dans « N lignes
					// atteignant le board » (3.4).
					if(n > summons.size()) {
						++summon_never;
						cons_ok = false;
						continue;
					}
					uint32_t canon = db.Canonical(summons[n - 1]);
					if(std::find(allowed.begin(), allowed.end(), canon) ==
					   allowed.end())
						cons_ok = false;
				}
				for(size_t k = 0; k < cons.resolve_min.size(); ++k)
					if(resolves[k] < cons.resolve_min[k].min_count)
						cons_ok = false;
				// But principal, ou un des buts ALTERNATIFS (--fire : le
				// board sans les cartes sacrifiees pour contrer la menace).
				const BoardKey fin = ComputeBoardKey(d, con);
				const bool full_board = fin == target;
				bool alt_board = false;
				if(!full_board && target_alts)
					for(const BoardKey& ab : *target_alts)
						if(fin == ab) {
							alt_board = true;
							break;
						}
				const bool ok = !retry && cons_ok && (full_board || alt_board);
				if(ok) {
					char name[64];
					std::snprintf(name, sizeof(name),
								  "solution_%02zu_b%u_a%u%s.yrp", i,
								  sols[i].burned, sols[i].actions,
								  full_board ? "" : "_alt");
					std::string path = outdir + "/" + name;
					std::string werr;
					if(WriteYrp1(path, start_yrp, sols[i].responses, werr)) {
						if(written == 0)
							first_summons = summons;
						++written;
					} else
						std::printf("  !! %s\n", werr.c_str());
				} else {
					++rejected;
					if(retry)
						++rej_retry;
					else if(!cons_ok) {
						++rej_cons;
						if(summon_never)
							++rej_never;
					}
					else
						++rej_board;
				}
				a.Restore();
			}
			a.Pop();
		}
		a.Shutdown();
	}).join();

	std::printf("\n--- sortie ---\n");
	std::printf("  %zu replay(s) ecrits dans %s  (sur %zu candidate(s))\n",
				written, outdir.c_str(), sols.size());
	if(sols.size() > kMaxWritten)
		std::printf("      !! %zu candidate(s) NON EXAMINEES : le plafond "
					"d'ecriture est de %zu.\n      Si l'appelant n'a pas trie, "
					"ce sont les premieres ARRIVEES, pas les moins cheres.\n",
					sols.size() - kMaxWritten, kMaxWritten);
	if(rejected)
		std::printf("  %zu rejetee(s) : %zu MSG_RETRY, %zu contrainte(s), "
					"%zu board non conforme\n", rejected, rej_retry, rej_cons,
					rej_board);
	if(rej_never)
		std::printf("      dont %zu ou une contrainte --summon n'est JAMAIS "
					"ATTEINTE :\n      la ligne se termine avant l'invocation "
					"visee, la contrainte n'a donc pas ete exercee\n", rej_never);
	if(cons.Any() && !first_summons.empty()) {
		std::printf("\n  invocations de la meilleure solution ecrite :\n");
		for(size_t i = 0; i < first_summons.size(); ++i) {
			bool constrained =
				cons.summons.count(static_cast<uint32_t>(i + 1)) != 0;
			std::printf("      #%-3zu %s%s\n", i + 1,
						db.Name(db.Canonical(first_summons[i])).c_str(),
						constrained ? "   <-- contrainte" : "");
		}
	}
	return written;
}

// PRIOR PAR REJEU DE SOLUTIONS (--prior, session 6 — arXiv:2401.10431,
// « Policy Learning from Solved Games ») : la politique NRPA vierge ne sait
// pas ripper (mesure : 1 tirage sur ~800 k fait les 3 resolutions exigees)
// alors que le corpus de solutions CONTIENT les sequences de rip completes.
// On releve les plan_key de chaque ligne du corpus — LiftPlan sur le duel de
// SON en-tete, jamais un rejeu sur le duel de depart (piege 21) ; seules les
// identites SEMANTIQUES traversent, invariantes par deck/main/graine — et on
// en fait des poids INITIAUX de politique : chaque worker demarre avec une
// politique qui sait deja ripper. Poids d'un coup : prior_weight x la
// proportion des fichiers du corpus qui le jouent — les coups presents
// PARTOUT (les rips, l'echine du combo) portent le poids plein, les
// idiosyncrasies d'une seule ligne un poids fractionnaire.
void BuildPriorPolicy(const Options& opt, CardDB& db, ScriptProvider& scripts,
					  NrpaPolicy& out) {
	if(opt.prior_files.empty())
		return;
	namespace fs = std::filesystem;
	std::vector<std::string> files;
	for(const std::string& p : opt.prior_files) {
		std::error_code ec;
		if(fs::is_directory(p, ec)) {
			for(const auto& e : fs::directory_iterator(p, ec)) {
				const auto ext = e.path().extension();
				if(ext == L".yrp" || ext == L".yrpX")
					files.push_back(e.path().string());
			}
		} else {
			files.push_back(p);
		}
	}
	std::sort(files.begin(), files.end());
	if(files.empty()) {
		std::printf("!! --prior : aucun fichier .yrp/.yrpX trouve\n");
		return;
	}
	std::printf("\n--- prior par rejeu : %zu ligne(s) de corpus (--prior) ---\n",
				files.size());
	auto t0 = Clock::now();
	std::map<uint64_t, uint32_t> in_files;   // plan_key -> nb de fichiers
	size_t used = 0;
	for(const std::string& f : files) {
		Replay holder;
		std::string err;
		if(!holder.Load(f, err)) {
			std::printf("  !! %s : %s\n", f.c_str(), err.c_str());
			continue;
		}
		const Replay* pr = holder.IsStreamed() ? holder.Embedded() : &holder;
		if(!pr || pr->responses.empty()) {
			std::printf("  !! %s : pas de reponses lisibles\n", f.c_str());
			continue;
		}
		// Le duel de SON en-tete — un thread dedie, une arene ne s'initialise
		// jamais sur un thread qui en possede deja une.
		std::thread([&] {
			Arena pa;
			std::string aerr;
			if(!pa.Init(opt.arena_mb << 20, 0, aerr)) {
				std::printf("  !! arene du prior : %s\n", aerr.c_str());
				return;
			}
			{
				Duel pd(db, scripts, &pa);
				if(!pd.Create(pr->seed, pr->duel_flags, pr->start_lp,
							  pr->start_hand, pr->draw_count, aerr) ||
				   !pd.Setup(*pr, aerr)) {
					std::printf("  !! %s : duel non initialisable : %s\n",
								f.c_str(), aerr.c_str());
				} else {
					if(opt.stop_gc)
						pd.SetLuaGc(false);
					EnumOptions eo;
					eo.dedup_by_code = true;
					eo.max_subsets = opt.max_subsets;
					eo.db = &db;
					std::vector<PlanStep> steps;
					size_t unknown = LiftPlan(pd, pa, *pr, opt.target_player,
											  SIZE_MAX, eo, steps);
					std::vector<uint64_t> distinct;
					for(const PlanStep& s : steps)
						if(s.edge)
							distinct.push_back(s.edge);
					std::sort(distinct.begin(), distinct.end());
					distinct.erase(
						std::unique(distinct.begin(), distinct.end()),
						distinct.end());
					for(uint64_t k : distinct)
						++in_files[k];
					if(!distinct.empty())
						++used;
					std::printf("  %-44s %4zu etapes, %3zu non identifiees, "
								"%zu coups distincts\n",
								fs::path(f).filename().string().c_str(),
								steps.size(), unknown, distinct.size());
				}
			}
			pa.Shutdown();
		}).join();
	}
	if(in_files.empty() || !used) {
		std::printf("  !! prior vide : aucun coup releve\n");
		return;
	}
	for(const auto& [k, n] : in_files)
		out[k] = static_cast<float>(opt.prior_weight) *
				 static_cast<float>(n) / static_cast<float>(used);
	std::printf("  prior : %zu coups distincts sur %zu ligne(s), poids max "
				"%.2f (--prior-weight), %.0f ms\n",
				in_files.size(), used, opt.prior_weight, MsSince(t0));
}

// REJEU D'ADAPTATION DU CORPUS (--adapt, session 7, chantier 5bis) — la voie
// restante de arXiv:2401.10431 apres la refutation, session 6, du prior par
// POIDS. Meme corpus, meme releve sur le duel de SON en-tete (piege 21), meme
// point d'injection (politique initiale des tirages) : ce qui change est la
// FORME du signal. Le prior disait « ce coup existe dans les solutions » et le
// primait partout ; l'adaptation dit « a CE carrefour, la solution prenait
// celui-ci contre ceux-la » — c'est le gradient NRPA lui-meme, applique aux
// sequences deja resolues au lieu des tirages du run.
//
// L'A/B est donc propre : --prior et --adapt injectent au meme endroit, la
// seule difference mesurable est prime-par-coup contre gradient discriminatif.
// --- APPRENTISSAGE DES LANDMARKS (chantier 18, session 16) -------------------
//
// Rejoue chaque plan RESOLU du corpus et releve, a chaque decision, le
// multiensemble des faits (code, zone). Le graphe fait ensuite l'INTERSECTION
// et l'ordre moyen (cf. LandmarkGraph).
//
// POURQUOI CE N'EST PAS `--adapt` SOUS UN AUTRE NOM. `--adapt` releve des
// CARREFOURS — quels coups etaient legaux, lequel a ete joue — et les verse au
// gradient de la politique. Il apprend a REPRODUIRE des lignes, et §9.14 a
// mesure son plafond : un poids par coup, aveugle a l'etat, accord du corpus
// bloque a 59 sur un maximum de memorisation pure a 64. Les landmarks relevent
// des ETATS : ce qu'il faut avoir eu, et combien de fois. C'est la difference
// entre « rejoue ce coup ici » et « il te faut deux Leo Dancer au cimetiere
// avant d'esperer trois Liger », et c'est la seconde qui se transporte a un
// etat que le corpus n'a jamais visite.
void BuildLandmarkGraph(const Options& opt, CardDB& db, ScriptProvider& scripts,
						const std::vector<PlanStep>& plan,
						const BoardKey& target, LandmarkGraph& graph) {
	if(opt.landmark_files.empty())
		return;
	namespace fs = std::filesystem;
	std::vector<std::string> files;
	for(const std::string& p : opt.landmark_files) {
		std::error_code ec;
		if(fs::is_directory(p, ec)) {
			for(const auto& e : fs::directory_iterator(p, ec)) {
				const auto ext = e.path().extension();
				if(ext == L".yrp" || ext == L".yrpX")
					files.push_back(e.path().string());
			}
		} else {
			files.push_back(p);
		}
	}
	std::sort(files.begin(), files.end());
	if(files.empty()) {
		std::printf("!! --landmarks : aucun fichier .yrp/.yrpX trouve\n");
		return;
	}
	std::unordered_map<uint64_t, size_t> repertoire;
	for(size_t i = 0; i < plan.size(); ++i)
		if(plan[i].edge)
			repertoire.emplace(plan[i].edge, i);

	std::printf("\n--- landmarks appris : %zu plan(s) resolu(s) (--landmarks) ---\n",
				files.size());
	auto t0 = Clock::now();
	for(const std::string& f : files) {
		Replay holder;
		std::string err;
		if(!holder.Load(f, err)) {
			std::printf("  !! %s : %s\n", f.c_str(), err.c_str());
			continue;
		}
		const Replay* pr = holder.IsStreamed() ? holder.Embedded() : &holder;
		if(!pr || pr->responses.empty()) {
			std::printf("  !! %s : pas de reponses lisibles\n", f.c_str());
			continue;
		}
		// Le duel de SON en-tete, sur un thread dedie : une arene ne
		// s'initialise jamais sur un thread qui en possede deja une.
		std::thread([&] {
			Arena pa;
			std::string aerr;
			if(!pa.Init(opt.arena_mb << 20, 0, aerr)) {
				std::printf("  !! arene des landmarks : %s\n", aerr.c_str());
				return;
			}
			{
				Duel pd(db, scripts, &pa);
				if(!pd.Create(pr->seed, pr->duel_flags, pr->start_lp,
							  pr->start_hand, pr->draw_count, aerr) ||
				   !pd.Setup(*pr, aerr)) {
					std::printf("  !! %s : duel non initialisable : %s\n",
								f.c_str(), aerr.c_str());
				} else {
					if(opt.stop_gc)
						pd.SetLuaGc(false);
					EnumOptions eo;
					eo.dedup_by_code = true;
					eo.max_subsets = opt.max_subsets;
					eo.db = &db;
					NrpaRun run;
					LandmarkTrace trace;
					LiftPolicyRun(pd, pa, *pr, opt.target_player, SIZE_MAX, eo,
								  repertoire, target, run, nullptr, 0, &trace);
					trace.Normalize();
					std::printf("  %-44s %4u decisions, %zu fait(s) distinct(s)\n",
								fs::path(f).filename().string().c_str(),
								trace.decisions, trace.first.size());
					if(trace.decisions)
						graph.AddPlan(std::move(trace));
				}
			}
			pa.Shutdown();
		}).join();
	}
	if(!graph.Plans()) {
		std::printf("  !! aucun plan exploitable : le graphe reste VIDE et le "
					"mecanisme sera inerte\n");
		return;
	}
	graph.Build();
	std::printf("  %zu plan(s) verses, %.0f ms\n", graph.Plans(), MsSince(t0));
	// LA RESERVE D'HONNETETE, imprimee et non enfouie. Avec un seul plan,
	// l'intersection EST ce plan : ce ne sont pas des landmarks generalises,
	// c'est la trace d'une ligne. Le dire ici evite qu'un A/B positif soit lu
	// comme une generalisation alors qu'il ne serait qu'un repertoire d'etats.
	if(graph.Plans() < 2)
		std::printf("  !! UN SEUL plan : l'intersection est ce plan. Les "
					"landmarks ne sont PAS generalises —\n     ils decrivent une "
					"ligne. A lire comme tel dans tout A/B.\n");
	if(graph.Empty()) {
		std::printf("  !! aucun landmark : tous les faits communs etaient deja "
					"vrais a l'etat initial (regle 1)\n");
		return;
	}
	// L'INSTRUMENT (piege 40 : chiffrer avant de laisser decider). Ce que le
	// graphe a appris, dans l'ordre de progression, avec les BOUCLES DE
	// REPETITION marquees — c'est la forme que le chantier demandait :
	// « Leo Dancer au cimetiere, COMPTE ».
	auto zname = [](uint8_t z) {
		switch(z) {
		case 0x02: return "main";
		case 0x10: return "cimetiere";
		case 0x20: return "banni";
		case 0x0c: return "terrain";
		case 0x82: return "ADV main";
		case 0x90: return "ADV cimet.";
		case 0xa0: return "ADV banni";
		case 0x8c: return "ADV terrain";
		default:   return "?";
		}
	};
	// Combien d'exemplaires au maximum par (code, zone) : un compte > 1 EST la
	// boucle de repetition du papier, et c'est la seule chose que le `h` plat
	// ne pouvait pas exprimer.
	std::map<uint64_t, uint32_t> loops;
	for(const Landmark& lm : graph.Items()) {
		uint32_t& m = loops[LandmarkGraph::KeyOf(lm.code, lm.zone)];
		m = (std::max)(m, lm.count);
	}
	size_t nloop = 0;
	for(const auto& [k, m] : loops)
		if(m > 1)
			++nloop;
	std::printf("  %zu landmark(s) sur %zu fait(s) distinct(s), dont %zu BOUCLE(S) "
				"DE REPETITION\n", graph.Items().size(), loops.size(), nloop);
	// Ordre de PREMIERE atteinte par (code, zone) : c'est l'axe du graphe, et
	// la table doit le suivre — triee par cle, elle rendrait l'ordre illisible
	// et la « progression ordonnee » ne serait qu'une promesse.
	std::vector<std::pair<float, uint64_t>> rows;
	for(const auto& [key, m] : loops) {
		float first = 2.0f;
		for(const Landmark& lm : graph.Items())
			if(LandmarkGraph::KeyOf(lm.code, lm.zone) == key)
				first = (std::min)(first, lm.order);
		rows.push_back({ first, key });
	}
	std::sort(rows.begin(), rows.end());
	auto label = [&](uint64_t key) {
		return db.Name(LandmarkGraph::CodeOf(key)) + " @" +
			   zname(LandmarkGraph::ZoneOf(key));
	};
	std::printf("     %-6s %-44s %-12s %s\n", "ordre", "landmark", "zone",
				"exemplaires");
	for(const auto& [first, key] : rows) {
		// Une ligne par (code, zone), portant l'ordre de son PREMIER
		// exemplaire et le compte exige : c'est lisible, la ou une ligne par
		// (code, zone, k) noierait la boucle dans ses propres repetitions.
		std::printf("     %-6.2f %-44s %-12s x%u%s\n", first,
					db.Name(LandmarkGraph::CodeOf(key)).c_str(),
					zname(LandmarkGraph::ZoneOf(key)), loops[key],
					loops[key] > 1 ? "   <-- BOUCLE" : "");
	}
	// L'ARETE DE PROGRESSION que le chantier nomme : pour chaque boucle, les
	// landmarks qui la PRECEDENT — c'est la phrase « il en faut un avant
	// chaque X », rendue verifiable. Les predecesseurs IMMEDIATS seulement
	// (les quatre derniers avant elle) : la liste complete serait la moitie du
	// graphe et ne dirait plus rien.
	for(const auto& [first, key] : rows) {
		if(loops[key] < 2)
			continue;
		std::printf("     boucle « %s x%u » — precedee de :",
					label(key).c_str(), loops[key]);
		std::vector<std::string> before;
		for(const auto& [o2, k2] : rows) {
			if(k2 == key || o2 >= first)
				continue;
			before.push_back(label(k2));
		}
		if(before.size() > 4)
			before.erase(before.begin(),
						 before.end() - 4);   // les plus PROCHES avant elle
		for(size_t i = 0; i < before.size(); ++i)
			std::printf("%s %s", i ? "," : "", before[i].c_str());
		std::printf("%s\n", before.empty() ? " (rien)" : "");
	}
}

void BuildAdaptRuns(const Options& opt, CardDB& db, ScriptProvider& scripts,
					const std::vector<PlanStep>& plan, const BoardKey& target,
					std::vector<NrpaRun>& out, RecipeGraph* recipes = nullptr) {
	if(opt.adapt_files.empty())
		return;
	namespace fs = std::filesystem;
	std::vector<std::string> files;
	for(const std::string& p : opt.adapt_files) {
		std::error_code ec;
		if(fs::is_directory(p, ec)) {
			for(const auto& e : fs::directory_iterator(p, ec)) {
				const auto ext = e.path().extension();
				if(ext == L".yrp" || ext == L".yrpX")
					files.push_back(e.path().string());
			}
		} else {
			files.push_back(p);
		}
	}
	std::sort(files.begin(), files.end());
	if(files.empty()) {
		std::printf("!! --adapt : aucun fichier .yrp/.yrpX trouve\n");
		return;
	}
	// Le repertoire de la reference, tel que l'echantillonnage le voit : Adapt()
	// doit recalculer les MEMES probabilites que PolicyRollout.
	std::unordered_map<uint64_t, size_t> repertoire;
	for(size_t i = 0; i < plan.size(); ++i)
		if(plan[i].edge)
			repertoire.emplace(plan[i].edge, i);

	std::printf("\n--- rejeu d'adaptation : %zu ligne(s) de corpus (--adapt) ---\n",
				files.size());
	auto t0 = Clock::now();
	size_t total_steps = 0, total_unknown = 0;
	for(const std::string& f : files) {
		Replay holder;
		std::string err;
		if(!holder.Load(f, err)) {
			std::printf("  !! %s : %s\n", f.c_str(), err.c_str());
			continue;
		}
		const Replay* pr = holder.IsStreamed() ? holder.Embedded() : &holder;
		if(!pr || pr->responses.empty()) {
			std::printf("  !! %s : pas de reponses lisibles\n", f.c_str());
			continue;
		}
		// Le duel de SON en-tete, sur un thread dedie : une arene ne s'initialise
		// jamais sur un thread qui en possede deja une.
		std::thread([&] {
			Arena pa;
			std::string aerr;
			if(!pa.Init(opt.arena_mb << 20, 0, aerr)) {
				std::printf("  !! arene de l'adaptation : %s\n", aerr.c_str());
				return;
			}
			{
				Duel pd(db, scripts, &pa);
				if(!pd.Create(pr->seed, pr->duel_flags, pr->start_lp,
							  pr->start_hand, pr->draw_count, aerr) ||
				   !pd.Setup(*pr, aerr)) {
					std::printf("  !! %s : duel non initialisable : %s\n",
								f.c_str(), aerr.c_str());
				} else {
					if(opt.stop_gc)
						pd.SetLuaGc(false);
					EnumOptions eo;
					eo.dedup_by_code = true;
					eo.max_subsets = opt.max_subsets;
					eo.db = &db;
					NrpaRun run;
					size_t unknown =
						LiftPolicyRun(pd, pa, *pr, opt.target_player, SIZE_MAX,
									  eo, repertoire, target, run, recipes);
					if(!run.steps.empty()) {
						total_steps += run.steps.size();
						total_unknown += unknown;
						std::printf("  %-44s %4zu decisions multi-choix, "
									"%2zu non identifiees\n",
									fs::path(f).filename().string().c_str(),
									run.steps.size(), unknown);
						out.push_back(std::move(run));
					} else {
						std::printf("  %-44s aucune decision relevee\n",
									fs::path(f).filename().string().c_str());
					}
				}
			}
			pa.Shutdown();
		}).join();
	}
	if(out.empty()) {
		std::printf("  !! adaptation vide : aucune sequence relevee\n");
		return;
	}
	// INSTRUMENTATION AVANT CALIBRAGE (piege 40) : la politique reproduit-elle
	// vraiment les sequences du corpus apres les passes ? A politique vierge,
	// l'accord vaut la moyenne des -log(nb de choix legaux) ; s'il ne monte pas,
	// le mecanisme est inerte et aucun run n'a besoin de le dire.
	std::printf("  adaptation : %zu ligne(s), %zu decisions (%zu non "
				"identifiees), %.0f ms\n",
				out.size(), total_steps, total_unknown, MsSince(t0));
	// COURBE DE SATURATION, calculee avant tout run (piege 40 : instrumenter
	// AVANT de calibrer). `p(choisi)` est la probabilite moyenne que la
	// politique donne aux coups que les solutions ont joues : a politique
	// vierge elle vaut la moyenne des 1/(nb de choix legaux), et si elle ne
	// monte pas avec les passes, le mecanisme est inerte — inutile de payer un
	// run de 600 s pour l'apprendre. Le nombre de poids dit combien de coups
	// distincts le corpus touche.
	SearchConfig defaults;
	{
		size_t ctx_span = 0;
		std::map<uint16_t, size_t> by_ctx;
		for(const NrpaRun& r : out)
			for(const PolicyStep& s : r.steps)
				++by_ctx[s.ctx];
		ctx_span = by_ctx.size();
		std::printf("  contextes distincts : %zu (board cible pose x main "
					"restante)\n", ctx_span);
	}
	// LE PLAFOND, mesure avant toute courbe. Deux etapes qui presentent le MEME
	// ensemble de coups legaux dans le MEME contexte sont indiscernables pour
	// une politique de cette famille ; si le corpus y joue des coups
	// differents, l'ecart est IRREDUCTIBLE. On rend donc la fraction d'etapes
	// qui jouent le coup majoritaire de leur point de decision : c'est ce
	// qu'atteindrait une TABLE parfaite sur ces points — une borne superieure
	// (le modele a poids PARTAGES entre points de decision ne l'atteint pas
	// forcement), mais une borne qui dit tout de suite si le corpus se
	// contredit ou si c'est l'apprentissage qui cale.
	{
		size_t g0 = 0, g1 = 0;
		const double c0 = CorpusCoherence(out, false, &g0);
		const double c1 = CorpusCoherence(out, true, &g1);
		std::printf("  plafond de la famille : %.1f%% sans contexte (%zu points "
					"de decision distincts) -> %.1f%% avec (%zu)\n",
					100.0 * c0, g0, 100.0 * c1, g1);
	}
	// La courbe d'accord, en TROIS dimensions : passes x pas d'adaptation x
	// niveau contextuel. Le pas y entre parce que la mise a jour NRPA
	// (+alpha au coup joue, -alpha*p a chacun) EST la montee de gradient de la
	// log-vraisemblance du corpus sous softmax : trop grand, elle oscille
	// autour de l'optimum au lieu de l'atteindre. C'est la recette « slow and
	// long adaptation » de Montparnasse (2505.02110), ici mesurable en
	// millisecondes au lieu d'un run.
	const float alphas[] = { 1.0f, 0.5f, 0.2f, 0.1f, 0.05f };
	for(int lvl = 0; lvl < 2; ++lvl) {
		const float k = lvl ? 1.0f : -1.0f;
		std::printf("  accord du corpus — niveau contextuel %s :\n",
					lvl ? "ACTIF (k=1)" : "eteint");
		std::printf("      %-8s", "passes");
		for(float al : alphas)
			std::printf("  a=%-6.2f", al);
		std::printf("\n");
		for(uint32_t n : { 0u, 1u, 4u, 16u, 64u, 256u }) {
			std::printf("      %-8u", n);
			for(float al : alphas) {
				NrpaPolicy probe;
				NrpaResidual res;
				AdaptCorpus(probe, &res, out, n, al,
					defaults.nrpa_bias_known, kReportHintBias, k,
					kReportTemp);
				double am = 0;
				const double a = CorpusAgreement(probe, &res, out,
												 defaults.nrpa_bias_known, k, &am);
				std::printf("  %4.0f/%3.0f%%", 100.0 * std::exp(a), 100.0 * am);
			}
			std::printf("\n");
		}
	}
	// Contexte pousse a l'extreme : la SIGNATURE du point de decision lui-meme
	// (contexte + ensemble des coups legaux). Le niveau contextuel devient
	// alors une quasi-table sur les points de decision — il doit atteindre le
	// plafond. Ce que ce bras mesure n'est pas un reglage utilisable mais une
	// REPONSE : si lui seul touche le plafond, alors reproduire le corpus exige
	// de le MEMORISER, et aucune representation qui generalise ne le fera.
	{
		std::vector<NrpaRun> sig_runs = out;
		std::vector<uint64_t> sorted;
		for(NrpaRun& r : sig_runs) {
			for(PolicyStep& s : r.steps) {
				sorted = s.keys;
				std::sort(sorted.begin(), sorted.end());
				uint64_t sig = (s.cctx + 1) * 0x9e3779b97f4a7c15ull;
				for(uint64_t k : sorted)
					sig = (sig ^ k) * 0x100000001b3ull;
				// Ecrit dans `cctx`, le champ que le niveau contextuel LIT
				// depuis la session 14 — l'ecrire dans `ctx` ferait mesurer a
				// cette sonde un conditionnement que plus personne n'utilise.
				// Et sur 64 bits : la troncature en uint16 faisait entrer en
				// collision des points de decision distincts, ce qui SOUS-estimait
				// le plafond de memorisation qu'elle est censee mesurer.
				s.cctx = sig;
			}
		}
		std::printf("  accord du corpus — contexte = SIGNATURE du point de "
					"decision (memorisation) :\n      %-8s", "passes");
		for(float al : alphas)
			std::printf("  a=%-6.2f", al);
		std::printf("\n");
		for(uint32_t n : { 0u, 1u, 4u, 16u, 64u, 256u }) {
			std::printf("      %-8u", n);
			for(float al : alphas) {
				NrpaPolicy probe;
				NrpaResidual res;
				AdaptCorpus(probe, &res, sig_runs, n, al,
							defaults.nrpa_bias_known, kReportHintBias,
							1.0f, kReportTemp);
				double am = 0;
				const double a = CorpusAgreement(probe, &res, sig_runs,
												 defaults.nrpa_bias_known, 1.0f,
												 &am);
				std::printf("  %4.0f/%3.0f%%", 100.0 * std::exp(a), 100.0 * am);
			}
			std::printf("\n");
		}
	}
	// PRÉVISION DE COÛT — l'instrument qui décide si sqrt-LTS vaut d'être
	// écrit. La borne LTS (d/pi) est calculée sur les lignes DÉJÀ RÉSOLUES du
	// corpus, puis comparée à la somme des bornes par segment délimité par les
	// indices que le solveur produit déjà (cartes du board cible posées).
	// L'écart est le gain maximal du rerooting, connu AVANT d'implémenter.
	{
		std::printf("  prevision de cout de recherche (borne LTS, log10 "
					"d'expansions) :\n");
		std::printf("      %-22s %10s %12s %8s %12s\n", "politique",
					"monolithe", "decompose", "segments", "pire segment");
		struct Arm { const char* name; uint32_t passes; float k; };
		const Arm arms[] = {
			{ "vierge (repertoire)", 0, -1.0f },
			{ "adaptee 4 passes", 4, -1.0f },
			{ "adaptee + contexte", 4, 1.0f },
		};
		for(const Arm& a : arms) {
			NrpaPolicy probe;
			NrpaResidual res;
			AdaptCorpus(probe, &res, out, a.passes, defaults.nrpa_alpha,
						defaults.nrpa_bias_known, kReportHintBias, a.k,
						kReportTemp);
			const CostForecast f = ForecastSearchCost(
				probe, &res, out, defaults.nrpa_bias_known, a.k);
			std::printf("      %-22s %9.1f  %11.1f  %7.1f  %11.1f\n", a.name,
						f.log10_mono, f.log10_decomp, f.segments,
						f.worst_seg_log10);
		}
		std::printf("      (le decompose est le MEILLEUR cas du rerooting : le "
					"papier ajoute\n       un facteur pour l'incertitude du "
					"rerooter — c'est un plafond.)\n");
	}
	// PREVISION DU GAIN DES OPTIONS (chantier 17). Le seul levier identifie qui
	// touche l'EXPOSANT et non la base — et, comme alpha pour sqrt-LTS, il est
	// chiffrable AVANT d'ecrire le mecanisme (piege 40).
	{
		std::printf("  prevision du gain des OPTIONS (macros minees dans le "
					"corpus, politique uniforme) :\n");
		std::printf("      %-10s %-9s %-6s %10s %11s %10s %9s\n", "catalogue",
					"support", "long.", "decisions", "avec macros",
					"log10 d/pi", "absorbe");
		for(const auto& [cat, sup] : { std::pair<size_t, uint32_t>{ 16, 4 },
									   std::pair<size_t, uint32_t>{ 64, 3 },
									   std::pair<size_t, uint32_t>{ 256, 2 } }) {
			const OptionForecast f = ForecastOptionGain(out, cat, sup, 8);
			if(!f.lines || !f.options) {
				std::printf("      %-10zu %-9u  (aucune macro de ce support)\n",
							cat, sup);
				continue;
			}
			std::printf("      %-10zu %-9u %-6zu %9.0f %10.0f  %5.1f -> %-5.1f "
						"%7.0f%%\n", f.options, sup, f.max_len, f.depth_flat,
						f.depth_opt, f.log10_flat, f.log10_opt,
						100.0 * f.covered);
		}
		std::printf("      (borne BASSE : le catalogue entier est suppose "
					"propose a chaque decision,\n       et aucune macro n'est "
					"creditee de raccourcir une ligne que le corpus a jouee.)\n");
	}
	{
		NrpaPolicy probe;
		NrpaResidual res;
		AdaptCorpus(probe, &res, out, opt.adapt_passes, defaults.nrpa_alpha,
					defaults.nrpa_bias_known, kReportHintBias,
					static_cast<float>(opt.ctx_shrink), kReportTemp);
		std::printf("  retenu : %u passe(s), retenue k=%.1f%s, %zu poids "
					"globaux + %zu contextuels\n",
					opt.adapt_passes, opt.ctx_shrink,
					opt.ctx_shrink < 0 ? " (niveau contextuel ETEINT)" : "",
					probe.size(), res.size());
		if(!opt.adapt_passes)
			std::printf("  (--adapt-passes 0 : releve fait, adaptation "
						"DESACTIVEE — bras temoin de l'A/B)\n");
	}
}

// TEST ADVERSE (--fire) — la garde etait un proxy statique (« un contre est
// disponible a chaque fenetre ») ; ce mode joue la menace POUR DE VRAI : la
// carte est ajoutee a la main adverse, l'adversaire l'ACTIVE a chaque fenetre
// ou elle est legale (un essai par fenetre), et la recherche enracinee doit
// refermer le board depuis l'etat post-injection — board complet (contre
// gratuit, Crystal Wing) ou board sans la carte sacrifiee (--fire-spare :
// contrer par Zalen consomme Junk Signal, arbitrage du joueur).
//
// Alignement du rejeu sur le duel AUGMENTE : ajouter une carte jouable ouvre
// des fenetres adverses NOUVELLES (le core ne demande que s'il existe une
// reponse legale) — les reponses enregistrees se decalent. On rejoue donc par
// JOUEUR : nos reponses dans l'ordre du fichier ; a une fenetre adverse
// ENREGISTREE (d'autres reponses que la carte tiree y existent), le passe
// enregistre ; a une fenetre NOUVELLE (la carte tiree est la SEULE chainable,
// soit exactement 2 choix : elle + le passe), un passe synthetique. La passe
// de decouverte doit atteindre le board avec 0 retry — c'est la preuve
// d'alignement, exigee avant toute injection.
//
// La garde est volontairement ABSENTE de la recherche de refermeture : la
// menace vient d'etre depensee (une seule copie ajoutee). --resolve,
// --no-activate et --no-chain restent.
void RunFireTest(Duel& duel, Arena& arena, const Replay& yrp,
				 const Options& opt, const LineResult& ref, CardDB& db,
				 ScriptProvider& scripts, const LineConstraints& cons) {
	std::printf("\n=== test adverse : l'adversaire JOUE la menace (--fire) ===\n");
	uint32_t fire_code = 0;
	if(!ResolveCard(opt.fire_spec, db, "--fire", fire_code))
		return;
	std::vector<uint32_t> spare_codes;
	for(const std::string& spec : opt.fire_spare_specs) {
		uint32_t c = 0;
		if(!ResolveCard(spec, db, "--fire-spare", c))
			return;
		spare_codes.push_back(c);
	}
	if(!ref.have_target) {
		std::printf("!! pas de board cible (la ligne n'atteint pas la fin du "
					"tour 1)\n");
		return;
	}
	const auto con = static_cast<uint8_t>(opt.target_player);
	std::printf("  menace : %s — ajoutee a la main adverse, JOUEE a chaque "
				"fenetre legale\n", db.Name(fire_code).c_str());
	// No-chain propre a la continuation (--fire-no-chain) : mise en scene
	// d'un contreur precis. Doit survivre aux threads de recherche.
	std::vector<uint32_t> fire_no_chain;
	for(const std::string& spec : opt.fire_no_chain_specs) {
		uint32_t c = 0;
		if(!ResolveCard(spec, db, "--fire-no-chain", c))
			return;
		fire_no_chain.push_back(c);
		std::printf("  continuation : %s ne chaine JAMAIS (--fire-no-chain)\n",
					db.Name(c).c_str());
	}

	while(arena.Depth() > 1)
		arena.Pop();
	if(arena.Depth() == 0)
		arena.Push();
	arena.Restore();

	// --- 1. Les deux boards but : complet, et sans la carte sacrifiable.
	BoardKey target;
	{
		size_t at = 0;
		at += Advance(duel, yrp, at, ref.target_at);
		target = ComputeBoardKey(duel, con);
		arena.Restore();
	}
	// Buts alternatifs : le board cible MOINS chaque SOUS-ENSEMBLE non vide
	// des cartes sacrifiables — une ligne qui contre en ne depensant que
	// Junk Signal doit matcher, comme une ligne qui perd aussi la piece de
	// construction que cette depense a cassee.
	std::vector<BoardKey> alts;
	if(!spare_codes.empty()) {
		if(spare_codes.size() > 3) {
			std::printf("!! --fire-spare : au plus 3 cartes\n");
			return;
		}
		const size_t n = spare_codes.size();
		for(size_t mask = 1; mask < (size_t(1) << n); ++mask) {
			auto mz = ref.target_self.mzone;
			auto sz = ref.target_self.szone;
			bool all_removed = true;
			std::string names;
			for(size_t i = 0; i < n; ++i) {
				if(!(mask & (size_t(1) << i)))
					continue;
				bool removed = false;
				for(auto* zone : { &mz, &sz }) {
					for(auto& c : *zone)
						if(c.present &&
						   db.Canonical(c.Code()) == spare_codes[i]) {
							c.present = false;
							removed = true;
							break;
						}
					if(removed)
						break;
				}
				if(!removed) {
					all_removed = false;
					break;
				}
				if(!names.empty())
					names += " + ";
				names += db.Name(spare_codes[i]);
			}
			if(!all_removed)
				continue;
			alts.push_back(MakeBoardKey(mz, sz, db));
			std::printf("  but alternatif : le board SANS %s\n", names.c_str());
		}
		if(alts.empty())
			std::printf("  !! --fire-spare : aucune des cartes n'est sur le "
						"board cible — buts alternatifs ignores\n");
	}
	const bool have_alt = !alts.empty();

	// --- 1bis. Variante VISIONNABLE (--fire-bake) : la carte tiree est CUITE
	// dans l'en-tete — inseree dans le deck adverse la ou le pseudo-melange
	// sert la main (la queue de la liste : le core pioche sur le dessus).
	// start_hand etant partage entre les deux joueurs, la carte prend la
	// place de la derniere carte de la main adverse d'origine (deplacee vers
	// le deck) : le duel differe du mode par defaut d'UNE carte de main
	// adverse, et la preuve d'alignement decide s'il reste rejouable. En
	// echange, les replays produits se rejouent DEPUIS LEUR FICHIER — EDOPro
	// les visionne sans drapeau.
	Replay baked;
	const Replay* fyrp = &yrp;
	const std::vector<uint32_t> fire_hand_v{ fire_code };
	const std::vector<uint32_t>* extra = &fire_hand_v;
	const uint8_t oppo = static_cast<uint8_t>(1 - opt.target_player);
	if(opt.fire_bake) {
		CopyReplayHeader(yrp, baked);
		bool in_hand = false;
		// Essai 1 : queue de la liste (le dessus du deck) ; essai 2 : tete.
		for(int attempt = 0; attempt < 2 && !in_hand; ++attempt) {
			baked.decks = yrp.decks;
			if(baked.decks.size() <= oppo)
				baked.decks.resize(oppo + 1);
			auto& main = baked.decks[oppo].main;
			if(attempt == 0)
				main.push_back(fire_code);
			else
				main.insert(main.begin(), fire_code);
			auto got = OpeningHand(baked, oppo, db, scripts, opt.arena_mb);
			for(uint32_t c : got)
				if(c == fire_code)
					in_hand = true;
		}
		if(!in_hand) {
			std::printf("!! --fire-bake : la carte n'arrive pas en main "
						"adverse par la donne (sonde) — abandon\n");
			return;
		}
		std::printf("  --fire-bake : %s cuit dans l'en-tete (deck adverse), "
					"verifie en main par sonde ;\n  les replays produits se "
					"visionnent dans EDOPro tels quels.\n",
					db.Name(fire_code).c_str());
		fyrp = &baked;
		extra = nullptr;
	}

	// --- 2. Le repertoire de la reference guide la refermeture.
	std::vector<PlanStep> plan;
	{
		EnumOptions eo;
		eo.dedup_by_code = true;
		eo.max_subsets = opt.max_subsets;
		eo.db = &db;
		// La valeur de retour est le nombre d'etapes NON IDENTIFIEES. Les trois
		// autres sites l'impriment ; ici elle etait jetee, et un plan a 90 % de
		// trous servait de repertoire comme s'il etait complet — l'echec des
		// fenetres etant alors impute a la recherche (C14).
		const size_t unknown = LiftPlan(duel, arena, yrp, opt.target_player,
										ref.target_at, eo, plan);
		std::printf("  repertoire de refermeture : %zu etape(s), "
					"%zu non identifiee(s)%s\n", plan.size(), unknown,
					(plan.size() && unknown * 2 > plan.size())
						? "   <-- le repertoire est majoritairement aveugle"
						: "");
		arena.Restore();
	}
	// Prior par rejeu (--prior) : politique INITIALE des recherches par
	// fenetre. Le mur mesure des fenetres precoces est la re-derivation des
	// trois rips depuis l'etat post-injection — exactement ce que le corpus
	// encode. Doit survivre aux threads de recherche.
	NrpaPolicy prior_policy;
	BuildPriorPolicy(opt, db, scripts, prior_policy);
	// Rejeu d'adaptation (--adapt, chantier 5bis) : meme corpus, meme point
	// d'injection, signal discriminatif au lieu d'une prime par coup.
	std::vector<NrpaRun> adapt_runs;
	BuildAdaptRuns(opt, db, scripts, plan, target, adapt_runs);

	// --- 3. Etiquetage par joueur : le rejeu augmente ne peut pas consommer
	// la liste plate (les fenetres nouvelles decalent tout).
	std::vector<std::vector<uint8_t>> ours, theirs;
	{
		size_t used = 0;
		int pplayer = -1;
		bool retry = false;
		bool done = false;
		while(!done) {
			int st = duel.Process();
			for(const Message& m : duel.Messages()) {
				if(m.type == MSG_RETRY)
					retry = true;
				if(IsPrompt(m.type))
					pplayer = m.size ? m.data[0] : -1;
			}
			if(retry)
				break;
			if(st == OCG_DUEL_STATUS_AWAITING) {
				if(used >= ref.target_at || used >= yrp.responses.size())
					break;
				if(pplayer == opt.target_player)
					ours.push_back(yrp.responses[used]);
				else
					theirs.push_back(yrp.responses[used]);
				duel.SetResponse(yrp.responses[used]);
				++used;
			} else if(st != OCG_DUEL_STATUS_CONTINUE)
				done = true;
		}
		arena.Restore();
		if(retry) {
			std::printf("!! etiquetage : MSG_RETRY sur le rejeu de base — "
						"abandon\n");
			return;
		}
		std::printf("  ligne de base : %zu reponses a nous, %zu passes "
					"adverses, board a la reponse %zu\n",
					ours.size(), theirs.size(), ref.target_at);
	}

	// --- 4. Decouverte des fenetres d'injection sur le duel augmente, avec
	// preuve d'alignement (board atteint, 0 retry).
	struct FireWindow {
		size_t our_at = 0;      // nos decisions deja jouees a la fenetre
		uint32_t summons = 0, actions = 0, turns = 0;
		uint64_t resolved = 0;
		std::vector<std::vector<uint8_t>> prefix;   // reponses deja envoyees
		std::vector<uint8_t> inject;   // la reponse adverse qui JOUE la carte
		bool fresh = false;            // fenetre NOUVELLE (ouverte par l'ajout)
		// Etat de la chaine a la fenetre : 0 = chaine VIDE (la carte tiree
		// DEMARRE une chaine — la vraie menace) ; sinon le code de l'effet
		// au sommet (la carte serait chainee par-dessus).
		uint32_t over = 0;
	};
	std::vector<FireWindow> windows;
	bool aligned = false;
	std::thread([&] {
		Arena fa;
		std::string err;
		if(!fa.Init(opt.arena_mb << 20, 0, err))
			return WorkerAbort("arene (test adverse)", err);
		{
			Duel fd(db, scripts, &fa);
			if(fd.Create(fyrp->seed, fyrp->duel_flags, fyrp->start_lp,
						 fyrp->start_hand, fyrp->draw_count, err) &&
			   fd.Setup(*fyrp, err, extra, oppo)) {
				if(opt.stop_gc)
					fd.SetLuaGc(false);
				EnumOptions oeo;   // enumeration ADVERSE : brute, sans nos filtres
				oeo.dedup_by_code = true;
				oeo.max_subsets = opt.max_subsets;
				oeo.db = &db;
				std::vector<std::vector<uint8_t>> prefix;
				size_t oi = 0, ti = 0;
				uint8_t ptype = 0;
				int pplayer = -1;
				std::vector<uint8_t> ppayload;
				uint32_t summons = 0, actions = 0, turns = 0;
				uint64_t resolved = 0;
				// Profondeur de la chaine courante et effet au sommet : c'est
				// ce qui distingue une fenetre OUVERTE (la menace demarre une
				// chaine) d'une fenetre de reponse en pleine resolution.
				uint32_t chain_depth = 0, chain_top = 0;
				bool retry = false;
				auto scan = [&] {
					for(const Message& m : fd.Messages()) {
						switch(m.type) {
						case MSG_RETRY: retry = true; break;
						case MSG_NEW_TURN: ++turns; break;
						case MSG_SUMMONING:
						case MSG_SPSUMMONING: {
							++summons;
							++actions;
							if(!cons.resolve_min.empty() && m.size >= 4) {
								uint32_t c = 0;
								std::memcpy(&c, m.data, 4);
								if(c) {
									const uint32_t sc = db.Canonical(c);
									for(size_t k = 0; k < cons.resolve_min.size(); ++k)
										if(cons.resolve_min[k].on_summon &&
										   cons.resolve_min[k].code == sc)
											resolved += 1ull << (16 * k);
								}
							}
							break;
						}
						case MSG_FLIPSUMMONING: ++actions; break;
						case MSG_CHAINING: {
							++actions;
							++chain_depth;
							if(m.size >= 4)
								std::memcpy(&chain_top, m.data, 4);
							if(!cons.resolve_min.empty() && m.size >= 4) {
								uint32_t c = 0;
								std::memcpy(&c, m.data, 4);
								c = db.Canonical(c);
								const uint32_t loc = ChainingLocation(m.data, m.size);
								for(size_t k = 0; k < cons.resolve_min.size(); ++k)
									if(!cons.resolve_min[k].on_summon &&
									   cons.resolve_min[k].code == c &&
									   (!cons.resolve_min[k].zones ||
										(loc & cons.resolve_min[k].zones)))
										resolved += 1ull << (16 * k);
							}
							break;
						}
						case MSG_CHAIN_END:
							chain_depth = 0;
							chain_top = 0;
							break;
						default: break;
						}
						if(IsPrompt(m.type)) {
							ptype = m.type;
							pplayer = m.size ? m.data[0] : -1;
							ppayload.assign(m.data, m.data + m.size);
						}
					}
				};
				auto send = [&](const std::vector<uint8_t>& r) {
					fd.SetResponse(r);
					prefix.push_back(r);
				};
				for(;;) {
					int st = fd.Process();
					scan();
					if(retry || turns >= 2)
						break;
					if(st == OCG_DUEL_STATUS_AWAITING) {
						if(pplayer == opt.target_player) {
							if(oi >= ours.size())
								break;   // le board est fait : fin de la passe
							send(ours[oi++]);
							continue;
						}
						// Fenetre adverse : la carte tiree y est-elle jouable ?
						auto choices = Enumerate(
							ptype, ppayload.data(),
							static_cast<uint32_t>(ppayload.size()), oeo);
						int fire_at = -1;
						for(size_t i = 0; i < choices.size(); ++i)
							if(choices[i].card &&
							   db.Canonical(choices[i].card) == fire_code) {
								fire_at = static_cast<int>(i);
								break;
							}
						// NOUVELLE ssi la carte tiree est la seule chainable
						// (elle + le passe). Sinon la fenetre existait dans
						// l'enregistrement : son passe enregistre s'applique.
						const bool fresh =
							fire_at >= 0 && choices.size() == 2;
						// --fire-open : la menace doit DEMARRER une chaine —
						// les fenetres en pleine resolution sont ecartees.
						if(fire_at >= 0 &&
						   (!opt.fire_open || chain_depth == 0)) {
							FireWindow w;
							w.our_at = oi;
							w.summons = summons;
							w.actions = actions;
							w.turns = turns;
							w.resolved = resolved;
							w.prefix = prefix;
							w.inject = choices[static_cast<size_t>(fire_at)].response;
							w.fresh = fresh;
							w.over = chain_depth ? chain_top : 0;
							windows.push_back(std::move(w));
						}
						if(fresh) {
							send(choices.back().response);   // passe synthetique
						} else if(ti < theirs.size()) {
							send(theirs[ti++]);              // passe enregistre
						} else if(!choices.empty()) {
							send(choices.back().response);
						} else {
							std::vector<uint8_t> def;
							if(!DefaultResponse(ptype, ppayload.data(),
												static_cast<uint32_t>(ppayload.size()),
												def))
								break;
							send(def);
						}
					} else if(st != OCG_DUEL_STATUS_CONTINUE)
						break;
				}
				if(!retry && oi >= ours.size()) {
					BoardKey fin = ComputeBoardKey(fd, con);
					aligned = fin == target;
				}
			} else {
				std::printf("  !! duel augmente non initialisable : %s\n",
							err.c_str());
			}
		}
		ReportPoison("test adverse", fa);
		fa.Shutdown();
	}).join();

	if(!aligned) {
		std::printf("!! la passe de decouverte n'atteint pas le board sur le "
					"duel augmente\n   (desalignement du rejeu par joueur) — "
					"aucune injection tentee.\n");
		return;
	}
	std::printf("  alignement PROUVE : la ligne de base refait le board sur le "
				"duel augmente.\n");
	std::printf("  %zu fenetre(s) ou %s est jouable%s", windows.size(),
				db.Name(fire_code).c_str(),
				opt.fire_open ? " EN OUVERTURE DE CHAINE (--fire-open)" : "");
	{
		size_t open_n = 0;
		for(const FireWindow& w : windows)
			open_n += w.over ? 0 : 1;
		std::printf(" (%zu chaine vide, %zu par-dessus un effet)\n",
					open_n, windows.size() - open_n);
	}
	if(windows.empty())
		return;

	// --- 5. Une injection par fenetre, recherche enracinee vers le(s) but(s).
	std::printf("\n--- injections : %.0f s de recherche par fenetre ---\n",
				opt.fire_ms / 1000.0);
	uint64_t base_seed = opt.seed;
	if(!base_seed) {
		base_seed = static_cast<uint64_t>(
			std::chrono::high_resolution_clock::now().time_since_epoch().count());
		if(!base_seed)
			base_seed = 1;
	}
	struct FireResult {
		bool tried = false, converted = false, alt = false;
		uint32_t b = 0, a = 0, d = 0;
		uint32_t best_overlap = 0;
		uint64_t rollouts = 0;
	};
	std::vector<FireResult> results(windows.size());
	std::vector<Solution> all_sols;
	std::mutex mx;
	std::atomic<size_t> next{ 0 };
	unsigned threads = opt.threads
		? opt.threads
		: (std::max)(1u, std::thread::hardware_concurrency());
	unsigned nw = (std::min<unsigned>)(threads,
									   static_cast<unsigned>(windows.size()));
	std::vector<std::thread> pool;
	for(unsigned t = 0; t < nw; ++t) {
		pool.emplace_back([&] {
			Arena fa;
			std::string err;
			if(!fa.Init(opt.arena_mb << 20, 0, err))
				return WorkerAbort("arene (continuation --fire)", err);
			{
				Duel fd(db, scripts, &fa);
				if(fd.Create(fyrp->seed, fyrp->duel_flags, fyrp->start_lp,
							 fyrp->start_hand, fyrp->draw_count, err) &&
				   fd.Setup(*fyrp, err, extra, oppo)) {
					if(opt.stop_gc)
						fd.SetLuaGc(false);
					fa.Push();   // etat de depart du duel augmente
					for(;;) {
						size_t w = next.fetch_add(1);
						if(w >= windows.size())
							break;
						const FireWindow& W = windows[w];
						fa.Restore();
						// Rejeu aveugle du prefixe : les octets sont exacts
						// pour CE duel (ils viennent de la passe de decouverte).
						bool retry = false;
						size_t used = 0;
						while(used < W.prefix.size() && !retry) {
							int st = fd.Process();
							for(const Message& m : fd.Messages())
								if(m.type == MSG_RETRY)
									retry = true;
							if(st == OCG_DUEL_STATUS_AWAITING)
								fd.SetResponse(W.prefix[used++]);
							else if(st != OCG_DUEL_STATUS_CONTINUE)
								break;
						}
						if(retry || used < W.prefix.size()) {
							std::lock_guard<std::mutex> lk(mx);
							std::printf("  fenetre %2zu : !! prefixe non "
										"rejouable (%zu/%zu)\n", w, used,
										W.prefix.size());
							continue;
						}
						// TRAITER la derniere reponse du prefixe : avancer
						// jusqu'au prompt suivant (la fenetre de tir) AVANT de
						// poser l'injection. Sans cela, SetResponse(inject)
						// ECRASE la reponse pendante — l'injection se joue au
						// prompt d'avant, la recherche part d'un etat decale
						// d'une reponse, et le chemin assemble ne rejoue pas
						// (mesure : 16/16 MSG_RETRY a l'ecriture, divergence
						// « decalage de fenetres » a +3 de l'injection).
						{
							int st = 0;
							do {
								st = fd.Process();
								for(const Message& m : fd.Messages())
									if(m.type == MSG_RETRY)
										retry = true;
							} while(st == OCG_DUEL_STATUS_CONTINUE && !retry);
							if(retry || st != OCG_DUEL_STATUS_AWAITING) {
								std::lock_guard<std::mutex> lk(mx);
								std::printf("  fenetre %2zu : !! la fenetre de "
											"tir ne s'ouvre pas au rejeu\n", w);
								continue;
							}
						}
						// L'INJECTION : l'adversaire joue la carte. Controle de
						// validite AVANT la recherche : une reponse que le
						// core rejette ferait mourir chaque tirage au premier
						// pas (des millions de morts instantanees, best 0/8)
						// en se faisant passer pour une infaisabilite de jeu.
						// Push/Pop d'arene : la recherche exige la convention
						// « reponse posee, non traitee » — le controle ne doit
						// rien consommer.
						fd.SetResponse(W.inject);
						bool inj_retry = false;
						{
							fa.Push();
							int st = 0;
							uint8_t rtype = 0;
							std::vector<uint8_t> rpayload;
							do {
								st = fd.Process();
								for(const Message& m : fd.Messages()) {
									if(m.type == MSG_RETRY)
										inj_retry = true;
									if(IsPrompt(m.type)) {
										rtype = m.type;
										rpayload.assign(m.data,
														m.data + m.size);
									}
								}
							} while(st == OCG_DUEL_STATUS_CONTINUE &&
									!inj_retry);
							// Le DIAGNOSTIC decisif : que peut-on repondre a
							// la menace ? Les choix enumeres au premier prompt
							// apres l'injection, muselieres marquees.
							if(!inj_retry && st == OCG_DUEL_STATUS_AWAITING &&
							   rtype) {
								EnumOptions reo;
								reo.dedup_by_code = true;
								reo.max_subsets = opt.max_subsets;
								reo.db = &db;
								auto ropts = Enumerate(
									rtype, rpayload.data(),
									static_cast<uint32_t>(rpayload.size()),
									reo);
								std::string names;
								for(const auto& c : ropts) {
									if(!c.card)
										continue;
									if(!names.empty())
										names += ", ";
									names += db.Name(db.Canonical(c.card));
									for(uint32_t nc : fire_no_chain)
										if(db.Canonical(c.card) == nc)
											names += " [muselee]";
								}
								std::lock_guard<std::mutex> lk(mx);
								std::printf("  fenetre %2zu : reponses a la "
											"menace : %s\n", w,
											names.empty() ? "(passer seulement)"
														  : names.c_str());
							}
							fa.Pop();
						}
						if(inj_retry) {
							std::lock_guard<std::mutex> lk(mx);
							std::printf("  fenetre %2zu (dec. %3zu, inv. %2u) "
										": injection REJETEE par le core "
										"(activation illegale ici)\n",
										w, W.our_at, W.summons);
							continue;
						}
						SearchConfig fcfg;
						fcfg.target_player = opt.target_player;
						fcfg.max_decisions = FinisherDepth(
							static_cast<uint32_t>(ref.target_at * 3 / 2 + 32),
							W.prefix.size());
						fcfg.max_actions = 0;
						fcfg.time_limit_ms = opt.fire_ms;
						fcfg.max_nodes = 50000000;
						fcfg.max_solutions = 4;
						fcfg.enumeration.dedup_by_code = true;
						fcfg.enumeration.max_subsets = opt.max_subsets;
						fcfg.enumeration.db = &db;
						if(!cons.no_activate.empty())
							fcfg.enumeration.no_activate = &cons.no_activate;
						// PAS de no_chain GLOBAL ici : la regle du joueur est
						// « ne jamais annuler NOS PROPRES cartes » — elle
						// supposait le solitaire, ou toute chaine repond a nos
						// actions. Chainer sur la menace REELLE est le role
						// des gardes (mesure : avec le filtre, le contre par
						// Crystal Wing etait interdit d'enumeration et aucune
						// fenetre ne convertissait board complet). Le
						// --fire-no-chain, lui, s'applique : il met en scene
						// un contreur precis.
						if(!fire_no_chain.empty())
							fcfg.enumeration.no_chain = &fire_no_chain;
						fcfg.resolve_min = cons.resolve_min;
						// GARDE ABSENTE : la menace vient d'etre depensee.
						fcfg.initial_summons = W.summons;
						fcfg.initial_turns = W.turns;
						fcfg.initial_resolved = W.resolved;
						fcfg.hint_cards = cons.hints;
						for(const ResolveReq& req : cons.resolve_min)
							if(std::find(fcfg.hint_cards.begin(),
										 fcfg.hint_cards.end(), req.code) ==
							   fcfg.hint_cards.end())
								fcfg.hint_cards.push_back(req.code);
						if(have_alt)
							fcfg.target_alts = &alts;
						// Prior : la politique demarre en sachant ripper.
						if(!prior_policy.empty())
							fcfg.nrpa_init = &prior_policy;
						// Rejeu d'adaptation : elle demarre en sachant quel
						// coup la solution prenait a chaque carrefour.
						if(!adapt_runs.empty()) {
							fcfg.nrpa_adapt_runs = &adapt_runs;
							fcfg.nrpa_adapt_passes = opt.adapt_passes;
						}
						// Politique a deux niveaux (chantier 5ter).
						fcfg.ctx_shrink = static_cast<float>(opt.ctx_shrink);
						// Conditionnement par le chemin (MCPS) et plafond de la
						// table contextuelle : les deux doivent voyager ENSEMBLE,
						// sinon le contexte est calcule et jamais borne.
						fcfg.qhat_depth = opt.qhat_depth;
						fcfg.qhat_window = opt.qhat_window;
						fcfg.qhat_rho = opt.qhat_rho;
						fcfg.qhat_max_nodes = opt.qhat_nodes;
						fcfg.enumeration.canonical_zones = opt.canonical_zones;
						fcfg.goal_subset =
							opt.target_subset ||
							(!opt.target_specs.empty() && !opt.target_exact);
						fcfg.ctx_max = opt.ctx_max;
						fcfg.nrpa_temp = static_cast<float>(opt.nrpa_temp);
						Search fs(fd, fa, *fyrp, fcfg);
						fs.RunNrpa(target, plan,
								   base_seed + w * 0x9E3779B97F4A7C15ull + 1);
						const SearchStats& st = fs.Stats();
						FireResult r;
						r.tried = true;
						r.best_overlap = st.best_overlap;
						r.rollouts = st.rollout_count;
						const Solution* best = nullptr;
						for(const Solution& s : fs.Solutions())
							if(!best ||
							   std::tie(s.burned, s.actions, s.decisions) <
								   std::tie(best->burned, best->actions,
											best->decisions))
								best = &s;
						if(best) {
							r.converted = true;
							r.alt = best->alt;
							r.b = best->burned;
							r.a = best->actions;
							r.d = best->decisions +
								  static_cast<uint32_t>(W.prefix.size());
						}
						std::lock_guard<std::mutex> lk(mx);
						results[w] = r;
						// Les cartes cibles MANQUANTES a la crete : c'est ce
						// qui dit si le contre consomme une carte du board
						// (le but alternatif doit alors l'epargner aussi).
						std::string miss;
						if(!r.converted) {
							size_t a = 0, b = 0;
							int shown = 0;
							while(b < target.codes.size() && shown < 3) {
								if(a < st.best_board.size() &&
								   st.best_board[a] == target.codes[b]) {
									++a;
									++b;
								} else if(a < st.best_board.size() &&
										  st.best_board[a] < target.codes[b]) {
									++a;
								} else {
									if(!miss.empty())
										miss += ", ";
									miss += db.Name(target.codes[b]);
									++shown;
									++b;
								}
							}
							if(!miss.empty())
								miss = "  manque : " + miss;
						}
						const std::string wpos =
							W.over ? "sur " + db.Name(W.over)
								   : std::string("OUVERTE");
						std::printf("  fenetre %2zu (dec. %3zu, inv. %2u, %s) : "
									"%s  [%llu tirages, best %u/%zu]%s\n", w,
									W.our_at, W.summons, wpos.c_str(),
									r.converted
										? (r.alt ? "CONVERTIE (board SANS "
												   "carte(s) sacrifiee(s))"
												 : "CONVERTIE (board COMPLET)")
										: "pas convertie",
									(unsigned long long)r.rollouts,
									st.best_overlap, target.codes.size(),
									miss.c_str());
						bool self_checked = false;
						for(Solution s : fs.Solutions()) {
							std::vector<std::vector<uint8_t>> full = W.prefix;
							full.push_back(W.inject);
							full.insert(full.end(), s.responses.begin(),
										s.responses.end());
							// AUTO-CONTROLE de la premiere solution : rejouer
							// le chemin assemble depuis zero sur CE duel et
							// localiser toute divergence — un chemin qui ne se
							// rejoue pas ici ne s'ecrira pas non plus.
							if(!self_checked) {
								self_checked = true;
								fa.Restore();
								size_t fed = 0;
								bool sc_retry = false;
								uint8_t sc_ptype = 0;
								int sc_player = -1;
								std::vector<uint8_t> sc_payload;
								while(fed < full.size() && !sc_retry) {
									int st2 = fd.Process();
									for(const Message& m : fd.Messages()) {
										if(m.type == MSG_RETRY)
											sc_retry = true;
										if(IsPrompt(m.type)) {
											sc_ptype = m.type;
											sc_player = m.size ? m.data[0] : -1;
											sc_payload.assign(m.data,
															  m.data + m.size);
										}
									}
									if(st2 == OCG_DUEL_STATUS_AWAITING)
										fd.SetResponse(full[fed++]);
									else if(st2 != OCG_DUEL_STATUS_CONTINUE)
										break;
								}
								if(sc_retry || fed < full.size()) {
									// La reponse rejetee figure-t-elle parmi
									// les choix enumeres A FROID a ce prompt ?
									// Oui = l'ETAT diverge (meme prompt, autre
									// contenu) ; non = la reponse vient d'un
									// AUTRE prompt (decalage de fenetres).
									EnumOptions deo;
									deo.dedup_by_code = true;
									deo.max_subsets = opt.max_subsets;
									deo.db = &db;
									auto cold = Enumerate(
										sc_ptype, sc_payload.data(),
										static_cast<uint32_t>(sc_payload.size()),
										deo);
									const std::vector<uint8_t>& bad =
										full[fed ? fed - 1 : 0];
									bool listed = false;
									for(const auto& c : cold)
										if(c.response == bad) {
											listed = true;
											break;
										}
									std::printf("  fenetre %2zu : !! "
												"auto-controle DIVERGE a la "
												"reponse %zu/%zu (prefixe %zu, "
												"injection %zu) — prompt %u "
												"joueur %d, %zu choix a froid, "
												"reponse du chemin %s\n", w,
												fed, full.size(),
												W.prefix.size(),
												W.prefix.size() + 1,
												sc_ptype, sc_player,
												cold.size(),
												listed ? "LISTEE (etat "
														 "divergent)"
													   : "NON LISTEE (decalage "
														 "de fenetres)");
								}
							}
							s.responses = std::move(full);
							s.decisions += static_cast<uint32_t>(W.prefix.size());
							s.actions += W.actions;
							all_sols.push_back(std::move(s));
						}
					}
				} else {
					WorkerAbort("duel (continuation --fire)", err);
				}
			}
			ReportPoison("continuation --fire", fa);
			fa.Shutdown();
		});
	}
	for(auto& t : pool)
		t.join();

	// --- 6. Verdict global et ecriture (verification comprise, garde omise —
	// la menace est depensee ; --resolve/--no-activate re-verifies).
	size_t converted = 0, full_n = 0, alt_n = 0;
	for(const FireResult& r : results) {
		if(!r.converted)
			continue;
		++converted;
		if(r.alt)
			++alt_n;
		else
			++full_n;
	}
	std::printf("\n=== verdict --fire : %zu fenetre(s) sur %zu converties "
				"(%zu board complet, %zu sans la carte sacrifiee) ===\n",
				converted, windows.size(), full_n, alt_n);
	if(converted < windows.size())
		std::printf("  les fenetres non converties ne sont PAS des preuves "
					"d'absence : budget %0.f s\n  d'echantillonnage par "
					"fenetre — approfondir avec --fire-ms.\n", opt.fire_ms / 1000.0);
	if(!all_sols.empty()) {
		LineConstraints fcons = cons;
		fcons.guard.clear();
		fcons.guard_after = 0;
		fcons.guard_opp_hand_release = -1;
		if(opt.fire_bake)
			std::printf("\n  replays des refermetures (en-tete CUIT : "
						"rejouables depuis leur fichier, EDOPro compris) :\n");
		else
			std::printf("\n  replays des refermetures (rejouables en mode juge "
						"avec --opp-hand \"%u\") :\n", fire_code);
		WriteSolutions(all_sols, *fyrp, target, opt, db, scripts, opt.outdir,
					   fcons, extra, have_alt ? &alts : nullptr);
	}
	arena.Restore();
}

// TRANSPLANTATION — refaire le board de reference depuis un AUTRE deck.
//
// Le probleme n'est plus d'ameliorer une ligne connue mais d'en reconstruire
// une : les reponses enregistrees ne designent rien dans un duel dont ni le
// deck, ni la main, ni la graine ne coincident. Ce qu'on transporte, c'est
// l'INTENTION de la ligne (LiftPlan), et on s'en sert comme ordre de visite.
// s22, chantier 3 : le modele de bilan est PRETE a la recherche (cfg.balance)
// pour la re-serialisation depuis la frontiere — il doit survivre au bloc qui
// le construit. Statique de process, comme le graphe de landmarks.
static BalanceModel g_balance_model;
static bool g_balance_armed = false;

// Le RESULTAT d'un appel, pour la boucle interne (s23) : ce que le round
// suivant doit savoir — combien de solutions, et quelle ligne jointe
// reinjecter. Rempli sur le chemin de la cible POSEE (le mode des rounds).
struct TransplantOutcome {
	size_t solutions = 0;
	uint32_t best_overlap = 0;
	uint32_t joint_rp = 0, joint_overlap = 0;
	std::string joint_file;   // vide : rien d'ecrit ce round
};

// CE QUI PERSISTE ENTRE LES ROUNDS (s24, --carry — Go-Explore complet).
// L'archive globale (finisseur compris sous --archive-fin) et la politique
// NRPA fusionnee survivent a l'appel : le round suivant SEME ses workers de
// tirages avec ces cellules et demarre sa politique de finisseur sur ces
// poids. Les chemins restent valides d'un round a l'autre parce que le
// gabarit de depart est LE MEME (meme graine, meme main, meme duel) — la
// propriete que same_gabarit verifie pour les approches est ici structurelle.
struct RoundCarry {
	std::unordered_map<uint64_t, ArchiveEntry> archive;
	NrpaPolicy policy;
	unsigned policy_workers = 0;
};

void RunTransplantSolve(Duel& duel, const Replay& ref_yrp, const Replay& start_yrp,
						const Options& opt, Arena& arena, const LineResult& ref,
						CardDB& db, ScriptProvider& scripts, uint32_t patience,
						const LineConstraints& cons,
						TransplantOutcome* outres = nullptr,
						RoundCarry* carry = nullptr) {
	std::printf("\n=== transplantation du combo sur un autre deck ===\n");
	if(!cons.opp_hand.empty()) {
		std::printf("  main adverse   : +%zu carte(s) (--opp-hand) :",
					cons.opp_hand.size());
		for(uint32_t c : cons.opp_hand)
			std::printf(" %s;", db.Name(c).c_str());
		std::printf("\n                   les replays produits ne se rejouent "
					"qu'avec le meme --opp-hand\n");
	}
	while(arena.Depth() > 1)
		arena.Pop();
	if(arena.Depth() == 0)
		arena.Push();
	arena.Restore();

	const auto con = static_cast<uint8_t>(opt.target_player);

	// --- 1. Le board a refaire, et ce qu'il a coute a la reference.
	BoardKey target;
	uint32_t ref_actions = 0, ref_burned = 0;
	size_t ref_decisions = 0;
	{
		size_t at = 0;
		uint32_t acts = 0;
		at += Advance(duel, ref_yrp, at, ref.target_at, &acts);
		target = ComputeBoardKey(duel, con);
		ref_actions = acts;
		ref_decisions = at;
		ref_burned = duel.Count(con, LOCATION_GRAVE) + duel.Count(con, LOCATION_REMOVED);
		arena.Restore();
	}
	// --- 1b. Edition du board cible : retirer / exiger des cartes. On part
	// des cartes CAPTUREES au board de reference (positions, materiaux,
	// compteurs compris) et on recompose la cle — jamais de cle bricolee.
	// La cible REELLEMENT retenue, conservee pour le rapport. Le bloc de
	// diagnostic « tous les codes y sont » imprimait `ref.target_self` — le
	// board du GABARIT — sous l'etiquette « cible : ». Sous --no-ref ce n'est
	// PAS la cible, et c'est exactement l'instrument qui aurait du montrer que
	// la cible posee a une zone S/T VIDE (session 15).
	std::vector<QueriedCard> posed_mz, posed_sz;
	bool posed = false;
	if(cons.AnyBoardEdit()) {
		// --target : table RASE. Le board de la reference n'entre pas — c'est la
		// difference entre « editer la cible de la reference » et « poser une
		// cible ». Sans cela il faut une cascade de --board-remove qui, elle,
		// depend de ce que la reference avait pose (donc d'une reference).
		auto mz = cons.target_scratch ? std::vector<QueriedCard>{}
									  : ref.target_self.mzone;
		auto sz = cons.target_scratch ? std::vector<QueriedCard>{}
									  : ref.target_self.szone;
		bool edit_ok = true;
		if(cons.target_scratch && !cons.board_remove.empty()) {
			std::printf("!! --target et --board-remove sont exclusifs : le board "
						"cible est deja construit de zero\n");
			return;
		}
		for(uint32_t code : cons.board_remove) {
			bool found = false;
			for(auto* zone : { &mz, &sz }) {
				for(auto& c : *zone) {
					if(c.present && db.Canonical(c.Code()) == code) {
						c.present = false;
						found = true;
						break;
					}
				}
				if(found)
					break;
			}
			if(!found) {
				std::printf("!! --board-remove : %s n'est pas sur le board "
							"cible\n", db.Name(code).c_str());
				edit_ok = false;
			}
		}
		for(const auto& [code, pos] : cons.board_add) {
			QueriedCard c;
			c.present = true;
			c.code = code;
			c.position = pos;
			mz.push_back(c);   // les ajouts sont des monstres, en MZONE
		}
		if(!edit_ok)
			return;
		if(cons.target_scratch && mz.empty() && sz.empty()) {
			std::printf("!! --target : cible vide, rien a atteindre\n");
			return;
		}
		target = MakeBoardKey(mz, sz, db);
		posed_mz = mz;
		posed_sz = sz;
		posed = true;
		std::printf("\n--- board cible %s ---\n",
					cons.target_scratch ? "POSE (--target, sans reference)"
										: "EDITE");
		for(const auto& c : mz)
			if(c.present)
				std::printf("      MZONE %9u  %-36.36s %s\n", c.Code(),
							db.Name(c.Code()).c_str(), PosName(c.position));
		for(const auto& c : sz)
			if(c.present)
				std::printf("      SZONE %9u  %-36.36s %s\n", c.Code(),
							db.Name(c.Code()).c_str(), PosName(c.position));
		if(target.mzone_count > 6)
			std::printf("  !! %u monstres exiges : PLUS que les 6 zones "
						"utilisables, cible inatteignable\n", target.mzone_count);
	}

	std::printf("  cible          : %zu cartes\n", target.entries.size());
	std::printf("  reference      : %u actions, %zu decisions, %u cartes brulees\n",
				ref_actions, ref_decisions, ref_burned);

	// --- 2. Faisabilite. Une carte du board doit venir du deck principal ou de
	// l'extra du joueur : il n'existe aucune autre source. Si elle n'y est pas,
	// le board est hors d'atteinte et toute recherche serait du temps perdu.
	{
		const Deck& deck = start_yrp.decks[opt.target_player];
		std::map<uint32_t, uint32_t> avail;
		for(const auto* list : { &deck.main, &deck.extra })
			for(uint32_t c : *list)
				++avail[db.Canonical(c)];

		std::map<uint32_t, uint32_t> need;
		for(uint32_t c : target.codes)
			++need[c];

		std::vector<std::pair<uint32_t, uint32_t>> missing;
		for(const auto& [code, n] : need) {
			uint32_t have = avail.count(code) ? avail[code] : 0;
			if(have < n)
				missing.emplace_back(code, n - have);
		}
		std::printf("\n--- faisabilite : les cartes du board sont-elles dans ce deck ? ---\n");
		if(missing.empty()) {
			std::printf("  les %zu cartes du board cible sont presentes.\n",
						need.size());
		} else {
			for(const auto& [code, n] : missing)
				std::printf("  MANQUE %dx %9u  %s\n", n, code, db.Name(code).c_str());
			std::printf("\n  Ce deck ne contient pas les cartes du board : la cible est\n"
						"  hors d'atteinte, quelle que soit la ligne. Recherche annulee.\n");
			return;
		}
	}

	// --- 2b. Les cartes que la ligne a EMPRUNTEES en route. Le board peut etre
	// present dans le deck alors que les intermediaires ont disparu : c'est ce
	// qui rend le plan inapplicable, et c'est la vraie explication d'un echec.
	if(ref.have_target) {
		const Deck& deck = start_yrp.decks[opt.target_player];
		std::map<uint32_t, uint32_t> avail;
		for(const auto* list : { &deck.main, &deck.extra })
			for(uint32_t c : *list)
				++avail[db.Canonical(c)];

		std::map<uint32_t, uint32_t> engaged;
		for(const auto* zone : { &ref.target_self.grave_cards,
								 &ref.target_self.removed_cards,
								 &ref.target_self.mzone, &ref.target_self.szone })
			for(const auto& [code, n] : CodeCounts(*zone, db))
				engaged[code] += n;

		std::vector<std::pair<uint32_t, uint32_t>> gone;
		for(const auto& [code, n] : engaged) {
			uint32_t have = avail.count(code) ? avail[code] : 0;
			if(have < n)
				gone.emplace_back(code, n - have);
		}
		std::printf("\n--- cartes empruntees par la ligne, absentes de ce deck ---\n");
		if(gone.empty()) {
			std::printf("  aucune : le deck de depart peut fournir les %zu cartes "
						"que la ligne engage.\n", engaged.size());
		} else {
			for(const auto& [code, n] : gone)
				std::printf("  MANQUE %dx %9u  %s\n", n, code, db.Name(code).c_str());
			std::printf("\n  Le board est atteignable en principe, mais la ligne de\n"
						"  reference passait par ces cartes-la : son plan ne peut pas\n"
						"  etre suivi tel quel, il faudra un autre chemin.\n");
		}

		// La main d'ouverture decide de tout : deux decks proches mais des mains
		// differentes ne jouent pas le meme jeu.
		std::printf("\n--- mains d'ouverture ---\n");
		std::printf("  reference :");
		for(const auto& c : ref.start_self.hand_cards)
			if(c.present)
				std::printf(" %s;", db.Name(db.Canonical(c.Code())).c_str());
		std::printf("\n  depart    :");
		for(uint32_t code : OpeningHand(start_yrp, con, db, scripts, opt.arena_mb))
			std::printf(" %s;", db.Name(code).c_str());
		std::printf("\n");
	}

	// --- 3. Relever la ligne de reference en intentions.
	std::vector<PlanStep> plan;
	size_t unknown = 0;
	{
		EnumOptions eo;
		eo.dedup_by_code = true;
		eo.max_subsets = opt.max_subsets;
		eo.db = &db;
		arena.Restore();
		auto t0 = Clock::now();
		unknown = LiftPlan(duel, arena, ref_yrp, opt.target_player, ref.target_at,
						   eo, plan);
		double ms = MsSince(t0);
		arena.Restore();
		std::printf("\n--- plan releve sur la ligne de reference ---\n");
		std::printf("  etapes            : %zu  (%.0f ms)\n", plan.size(), ms);
		std::printf("  non identifiees   : %zu%s\n", unknown,
					unknown ? "   <-- autant de trous dans le guide" : "");
		std::map<uint8_t, size_t> by_type;
		for(const auto& s : plan)
			++by_type[s.prompt_type];
		for(const auto& [type, n] : by_type)
			std::printf("      %-24s %4zu\n", PromptName(type), n);

		// L'ouverture de la ligne dit ce que le nouveau deck doit savoir
		// reproduire. C'est la partie du plan qui echoue en premier.
		std::printf("\n  ouverture de la ligne :\n");
		size_t shown = 0;
		for(const auto& s : plan) {
			if(shown >= 18)
				break;
			// Les etapes structurelles (chaine vide, choix de zone) noient le
			// propos : on ne montre que ce qui engage une carte.
			if(s.prompt_type != MSG_SELECT_IDLECMD && s.prompt_type != MSG_SELECT_CARD)
				continue;
			uint32_t code = 0;
			if(std::sscanf(s.label.c_str(), "%*[^0-9]%u", &code) == 1 && code > 1000)
				std::printf("      %-14s %-18s %s\n", PromptName(s.prompt_type),
							s.label.c_str(), db.Name(db.Canonical(code)).c_str());
			else
				std::printf("      %-14s %s\n", PromptName(s.prompt_type),
							s.label.c_str());
			++shown;
		}
	}
	// MODE BUT SEUL : le repertoire est jete APRES avoir ete releve et imprime.
	// Le releve reste, pour que la mesure dise exactement CE QU'ON RETIRE — un
	// mecanisme neutralise en silence n'est pas un bras temoin.
	if(opt.no_plan) {
		std::printf("\n  --no-plan : les %zu etapes ci-dessus sont ECARTEES.\n"
					"  La politique NRPA demarre uniforme : plus de biais "
					"`known`, ni aux tirages\n  ni au finisseur. C'est le mode "
					"BUT SEUL — la reference ne sert plus que de\n  gabarit de "
					"duel%s.\n", plan.size(),
					opt.no_ref ? " (--no-ref : board cible et ligne ecartes aussi)"
							   : "");
		plan.clear();
	}
	if(plan.empty() && !opt.no_plan) {
		std::printf("\n  Plan vide : rien a transplanter.\n");
		return;
	}

	// Prior par rejeu (--prior) : poids INITIAUX de la politique des tirages,
	// releves sur le corpus de solutions (chaque ligne sur SON duel). Doit
	// survivre a toutes les phases (tirages, finisseur).
	NrpaPolicy prior_policy;
	BuildPriorPolicy(opt, db, scripts, prior_policy);
	// GRAPHE DE RECETTES (chantier 16). Cree AVANT le rejeu d'adaptation, pour
	// que les invocations du corpus le nourrissent en recettes OBSERVEES
	// (revue session 12) — materiaux et zones reels, voies d'exception
	// comprises : c'est ce qui casse l'oeuf-et-la-poule du graphe
	// observationnel sans lire un seul texte d'effet. Un seul graphe pour tout
	// le run : les recettes sont des FAITS, les reunir ne peut qu'enrichir.
	// `static` : il survit a toutes les recherches du run.
	static RecipeGraph recipe_graph;
	// Les sous-buts derives du bilan matiere sont calcules PLUS HAUT que la
	// construction du `SearchConfig` (l'amorce par operateurs precede la
	// recherche) : ils transitent par ici, et sont verses dans `cfg` au moment
	// ou il existe. Un seul point de versement, comme le chantier D l'exige.
	std::vector<SearchConfig::SerialReq> serial_from_balance;
	// Les hotes a QUOTA (s21, l'etude des briques) : verses dans cfg au meme
	// point que serial_from_balance.
	std::vector<uint32_t> quota_hosts_wiring;
	if(opt.probe_repeat && cons.resolve_min.empty())
		std::printf("!! --probe-repeat sans --summon-min ni --resolve : aucune "
					"carte a surveiller, la sonde restera MUETTE\n");
	if(opt.recipes >= 0) {
		std::printf("  graphe de recettes : ACTIF, poids %.2f%s\n",
					static_cast<float>(opt.recipes),
					opt.recipes == 0.0
						? "  (alimente et MESURE, n'entre pas dans le cout)"
						: "  (la distance de recettes pese dans h)");
		std::vector<uint32_t> watched;
		for(const ResolveReq& rq : cons.resolve_min)
			watched.push_back(rq.code);
		// AMORCE PAR LE TEXTE, sans quoi la carte jamais posee n'a aucune
		// recette et sa distance retombe au plancher — c'est-a-dire au `h`
		// plat.
		if(opt.seed_recipes && !start_yrp.decks.empty()) {
			const Deck& sd = start_yrp.decks[
				opt.target_player < static_cast<int>(start_yrp.decks.size())
					? opt.target_player : 0];
			const size_t n = SeedRecipesFromText(db, sd, target, recipe_graph,
												 opt.seed_cardinal, watched);
			std::printf("  amorce par le texte : %zu recette(s) posee(s), "
						"%zu produit(s) connus\n", n, recipe_graph.Products());
			ReportSeededDistances(target, recipe_graph, db, watched);
		}
		// CHANTIER 1 : L'AMORCE PAR LES OPERATEURS DECLARES.
		if(opt.op_recipes && !start_yrp.decks.empty()) {
			const Deck& sd = start_yrp.decks[
				opt.target_player < static_cast<int>(start_yrp.decks.size())
					? opt.target_player : 0];
			std::vector<uint32_t> codes;
			for(const auto* l : { &sd.main, &sd.extra })
				for(uint32_t c : *l)
					codes.push_back(c);
			for(uint32_t c : target.codes)
				codes.push_back(c);
			ConstantTable kt;
			if(!kt.Load(scripts)) {
				std::printf("!! --op-recipes : aucune constante lue (constant.lua "
							"absent des --scriptdir). L'amorce serait VIDE : elle "
							"est sautee plutot que de se declarer active.\n");
			} else {
				OperatorTable tbl;
				tbl.Build(db, scripts, kt, codes);
				// (a) LES RECETTES DECLAREES. `Fusion.AddProcMixN(c,...,24550676,
				//     1, IsSetCard(SET_LUNALIGHT), 3)` porte des CODES : plus de
				//     nom anglais a re-resoudre, plus de « route morte » deduite
				//     d'une phrase.
				std::vector<uint32_t> owned;
				for(const auto* l : { &sd.main, &sd.extra })
					for(uint32_t c : *l)
						owned.push_back(db.Canonical(c));
				std::sort(owned.begin(), owned.end());
				owned.erase(std::unique(owned.begin(), owned.end()), owned.end());
				size_t nrec = 0, dead = 0;
				for(const auto& [c, co] : tbl.All()) {
					for(const DeclaredRecipe& rc : co.recipes) {
						if(rc.named.empty() && rc.setcode.empty())
							continue;
						std::vector<Requirement> mats;
						bool route_morte = false;
						for(const auto& [mc, n] : rc.named) {
							if(!std::binary_search(owned.begin(), owned.end(),
												   db.Canonical(mc))) {
								route_morte = true;   // piege 63, inchange
								break;
							}
							for(uint32_t k = 0; k < n; ++k)
								mats.push_back(Requirement{ db.Canonical(mc),
															kZoneAny, kReqCard, 1 });
						}
						if(route_morte) { ++dead; continue; }
						if(opt.seed_cardinal)
							for(const auto& [sc, n] : rc.setcode)
								mats.push_back(Requirement{
									static_cast<uint32_t>(sc), kZoneAny,
									kReqSetcode, static_cast<uint8_t>(n) });
						if(mats.empty())
							continue;
						recipe_graph.Observe(c, mats, /*primed=*/true);
						++nrec;
					}
				}
				// (b) LE TYPE DE NŒUD MANQUANT : un CODE qu'on ACQUIERT.
				const std::vector<AcquirableCode> acq =
					AcquirableCodesOf(tbl, db, owned);
				// LA ZONE DE LA SOURCE EST CELLE OU LA CARTE SE TROUVE.
				// `Requirement::zone` est UN seau normalise : il ne sait pas
				// dire « DECK ou EXTRA ». Or l'operateur balaye les deux, et
				// collapser le masque en aveugle (NormalizeZone rend EXTRA pour
				// DECK|EXTRA) exigerait toutes les sources dans l'extra —
				// l'exigence deviendrait fausse pour les quatorze quinziemes
				// d'entre elles, en silence. On tranche par le DECK, qu'on a
				// sous la main : c'est un fait, pas une convention.
				auto in_list = [&db](const std::vector<uint32_t>& l, uint32_t c2) {
					for(uint32_t x : l)
						if(db.Canonical(x) == c2)
							return true;
					return false;
				};
				for(const AcquirableCode& a : acq) {
					uint8_t src = NormalizeZone(static_cast<uint8_t>(a.source_zone));
					if(in_list(sd.extra, a.code) && (a.source_zone & LOCATION_EXTRA))
						src = NormalizeZone(LOCATION_EXTRA);
					else if(in_list(sd.main, a.code) && (a.source_zone & LOCATION_DECK))
						src = NormalizeZone(LOCATION_DECK);
					std::vector<Requirement> mats;
					mats.push_back(Requirement{ a.host,
						NormalizeZone(static_cast<uint8_t>(a.host_range
														   ? a.host_range : 0x0c)),
						kReqCard, 1 });
					mats.push_back(Requirement{ a.code, src, kReqCard, 1 });
					recipe_graph.Observe(a.code, mats, /*primed=*/true);
				}
				std::printf("  amorce par les OPERATEURS : %zu recette(s) "
							"declaree(s) (%zu route(s) morte(s)), %zu arete(s) "
							"d'ACQUISITION de code, %zu produit(s) connus\n",
							nrec, dead, acq.size(), recipe_graph.Products());
				// La VIE du mecanisme, et elle est nominative : sans elle, une
				// amorce a zero arete se lirait comme une amorce active
				// (piege 42, deux sessions payees pour --assign-bias).
				for(const AcquirableCode& a : acq)
					std::printf("      %s peut ACQUERIR le code de %s  (%s, hote "
								"@%s, source @%s)\n", db.Name(a.host).c_str(),
								db.Name(a.code).c_str(), a.grant.c_str(),
								ZoneMaskName(static_cast<uint32_t>(a.host_range)).c_str(),
								ZoneMaskName(static_cast<uint32_t>(a.source_zone)).c_str());
				if(acq.empty())
					std::printf("      (aucune arete d'acquisition : aucune carte "
								"du deck n'accorde EFFECT_ADD_CODE)\n");
				ReportSeededDistances(target, recipe_graph, db, watched);

				// --- LA SERIALISATION PAR x* (9.31) ---------------------------
				//
				// L'arithmetique ne laisse qu'une voie : ~110 decisions reelles
				// d'arite geometrique 5,9 font `10^85` en UN bloc et `2x10^7` en
				// quatorze blocs de huit. Ce qui manquait n'etait pas le
				// mecanisme — `novelty_serialize` rouvre deja la table — mais
				// son CRITERE : « une carte cible posee » ne bouge qu'a la toute
				// fin. Les sous-buts du bilan matiere, eux, existent des la
				// premiere brique, et ils sont CALCULES (theoremes de 9.30), pas
				// devines.
				std::vector<std::pair<uint32_t, uint32_t>> gc2;
				for(uint32_t c : target.codes) {
					auto it = std::find_if(gc2.begin(), gc2.end(),
										   [&](const auto& g) {
											   return g.first == c;
										   });
					if(it == gc2.end())
						gc2.emplace_back(c, 1u);
					else
						++it->second;
				}
				// `owned` est DEDOUBLONNE (il sert d'ensemble d'appartenance) :
				// le passer au bilan matiere donnerait UNE copie par code au
				// lieu de trois, et « trois Liger » deviendrait infaisable pour
				// une raison qui n'a rien a voir avec le deck. Le modele veut
				// les copies PHYSIQUES.
				std::vector<uint32_t> deck_mult;
				for(const auto* l : { &sd.main, &sd.extra })
					for(uint32_t c : *l)
						deck_mult.push_back(c);
				// LES RESOLUTIONS SE COMPILENT DANS LE BILAN (s23). Le but a
				// deux moities de meme rang — l'etat final (--target) et les
				// passages exiges (--resolve/--summon-min) — et seule la
				// premiere etait compilee : tout le credit de la seconde etait
				// post-evenement, donc aucun barreau mi-ligne, donc la carte a
				// resoudre ne vivait que dans le spasme terminal (mesure :
				// 1re invocation d'Omega a 240,5 decisions, 12,9 de vie
				// restante, conjonction ~2e-8/tirage). UNE demande de presence
				// @DISPO par code (jamais N : le retour d'un meme corps est
				// legal, exiger 1 garde h admissible). Temoin : --resolve-legacy
				// rejoue le cablage s22quater a l'identique.
				std::vector<std::pair<uint32_t, uint32_t>> gtr;
				if(!opt.resolve_legacy)
					for(const ResolveReq& rr : cons.resolve_min) {
						const uint32_t cc = db.Canonical(rr.code);
						if(std::find_if(gtr.begin(), gtr.end(),
										[&](const auto& g) {
											return g.first == cc;
										}) == gtr.end())
							gtr.emplace_back(cc, 1u);
					}
				if(!cons.resolve_min.empty()) {
					std::string vie;
					for(const auto& g : gtr)
						vie += "x1 " + db.Name(g.first) + " @DISPO ; ";
					std::printf("  RESOLUTIONS -> BILAN : %s(cablage : "
								"%s — temoin via --resolve-legacy)\n",
								vie.c_str(),
								opt.resolve_legacy ? "LEGACY s22quater, demandes "
													 "NON posees"
												   : "demandes transitoires s23");
				}
				BalanceModel& bm = g_balance_model;
				g_balance_armed = false;
				if(opt.serial && !gc2.empty() &&
				   bm.Build(tbl, db, kt, deck_mult, gc2, gtr)) {
					LPResult lr;
					const double h0 = bm.Solve(deck_mult, {}, {}, &lr);
					if(lr.feasible && lr.primal_ok && lr.optimal_ok) {
						g_balance_armed = true;
						for(const BalanceModel::Need& n : bm.NeedsFrom(lr)) {
							if(n.zone == 0)
								continue;   // la RESERVE n'est pas un progres
							SearchConfig::SerialReq rq;
							rq.code = n.code;
							rq.arch = n.arch;
							rq.zone = n.zone;
							rq.count = n.count;
							serial_from_balance.push_back(rq);
						}
						// LES BARREAUX DE CONSOMMATION (s21). Le profil des
						// ecarts a montre que tout l'ASSEMBLAGE est invisible
						// aux sous-buts de production : trois deserts de 46 a
						// 73 reponses, chacun hors de portee d'un
						// echantillonneur (b^37 ~ 10^28). La colonne negative
						// de x*, rendue en arrivees @CIMETIERE, monte pendant
						// ces deserts — c'est le grain que la forme close
						// exige.
						for(const BalanceModel::Need& n : bm.ConsumedFrom(lr)) {
							SearchConfig::SerialReq rq;
							rq.code = n.code;
							rq.arch = n.arch;
							rq.zone = n.zone;
							rq.count = n.count;
							serial_from_balance.push_back(rq);
						}
						// LES DEPARTS DE RESERVE (s21, apres le nommage des
						// deserts). Les invocations intermediaires — les
						// vehicules d'extra deck que x* ne tire pas parce que
						// le LP fusionne « directement » — consomment toutes
						// la reserve : c'est la ressource IRREVERSIBLE du
						// tour, la seule grandeur qui monte REGULIEREMENT
						// pendant les deserts +46/+47 du profil des ecarts.
						// Une unite par carte sortie de deck+extra depuis la
						// racine ; plafond 15 = l'empaquetage (sur-compter est
						// anodin, une unite jamais atteinte ne cree pas de
						// cellule).
						// Deux TRANCHES de 15 (le champ `arch` porte le
						// decalage) : la ligne reelle fait ~25 departs, et la
						// premiere tranche seule saturait a la reponse 118 —
						// juste avant les deserts a couvrir (mesure au banc).
						for(uint64_t off : { 0ull, 15ull }) {
							SearchConfig::SerialReq rq;
							rq.zone = 5;
							rq.arch = off;
							rq.count = 15;
							serial_from_balance.push_back(rq);
						}
						// LES HABILITANTS ET LES QUOTAS : DEUX DERIVATIONS,
						// l'une CALCULEE (s22, les duaux — le defaut), l'autre
						// par CLASSES d'effets (s21 — le temoin, gardee le
						// temps d'une mesure : --quota-legacy).
						std::vector<uint32_t> quota_legacy;
						std::vector<SearchConfig::SerialReq> presence_legacy;
						std::unordered_map<uint32_t, uint32_t> dcop;
						for(uint32_t c : deck_mult)
							++dcop[db.Canonical(c)];
						uint64_t k_add = 0, k_efm = 0;
						kt.Lookup("EFFECT_ADD_CODE", k_add);
						kt.Lookup("EFFECT_EXTRA_FUSION_MATERIAL", k_efm);
						// (a) LE TEMOIN s21 : tout hote d'un EFFECT_ADD_CODE
						// ou d'un EFFECT_EXTRA_FUSION_MATERIAL present au deck
						// devient un barreau de PRESENCE @EN JEU et un hote a
						// quota.
						{
							for(const auto& kvv : tbl.All()) {
								const uint32_t host = db.Canonical(kvv.first);
								auto dit = dcop.find(host);
								if(dit == dcop.end())
									continue;
								bool enabler = false;
								for(const DeclaredEffect& g :
									kvv.second.grants)
									enabler = enabler ||
											  (g.code_is_effect &&
											   ((k_add &&
												 g.code_value == k_add) ||
												(k_efm &&
												 g.code_value == k_efm)));
								if(!enabler)
									continue;
								SearchConfig::SerialReq rq;
								rq.code = host;
								rq.zone = 6;   // EN JEU (MZONE + SZONE)
								rq.count = (std::min)(dit->second, 3u);
								presence_legacy.push_back(rq);
								// Un habilitant est aussi un hote a QUOTA.
								quota_legacy.push_back(host);
							}
						}
						// LES HOTES A QUOTA (s21, l'etude des briques) : aux
						// habilitants s'ajoutent les hotes d'un produit
						// d'INVOCATION SPECIALE dont la portee touche
						// l'archetype du but (la fusion de Wolf : ignition
						// 1/tour, membre invisible de la conjonction). Le
						// compte d'activations le long du chemin entre dans la
						// cle de cellule.
						{
							// LE CRITERE EST LA VOIE DU BUT, pas l'archetype :
							// le but est une FUSION, donc les quotas qui
							// comptent sont les IGNITEURS de Fusion
							// (CATEGORY_FUSION_SUMMON — la fusion de Wolf,
							// 1/tour depuis la PZONE). La premiere version
							// prenait tout produit SPECIAL_SUMMON d'archetype
							// et le plafond de 6 coupait... Wolf exactement.
							uint64_t c_fu = 0;
							kt.Lookup("CATEGORY_FUSION_SUMMON", c_fu);
							std::vector<uint64_t> goal_archs;
							for(const auto& [gc0, gn0] : gc2) {
								(void)gn0;
								if(const CardRow* row =
									   db.Find(db.Canonical(gc0)))
									for(uint16_t sc : row->setcodes)
										if(sc)
											goal_archs.push_back(sc & 0x0fffu);
							}
							for(const auto& kvv : tbl.All()) {
								const uint32_t host = db.Canonical(kvv.first);
								// (i) produit de FUSION declare ; ou (ii) un
								// effet BORNE (SetCountLimit) d'un hote dont
								// les series listees croisent l'archetype du
								// but — la fusion de Wolf vit dans Fusion.lua
								// partage, hors de portee de l'extraction par
								// carte : c'est son 1/tour + SET_LUNALIGHT qui
								// le designent.
								bool fu = false;
								for(const DeclaredProduct& pr :
									kvv.second.products)
									fu = fu ||
										 (c_fu && (pr.category & c_fu));
								bool capped_goal = false;
								bool series_goal = false;
								for(uint64_t s0 : kvv.second.listed_series)
									for(uint64_t ga : goal_archs)
										series_goal =
											series_goal ||
											((s0 & 0x0fffull) == ga);
								if(series_goal)
									for(const std::vector<DeclaredEffect>* set :
										{ &kvv.second.operators,
										  &kvv.second.grants })
										for(const DeclaredEffect& e0 : *set)
											capped_goal = capped_goal ||
														  e0.has_count_limit;
								if(!fu && !capped_goal)
									continue;
								// FILTRE DE PRINCIPE : un sort a usage unique
								// laisse une trace VISIBLE en zone (il part au
								// cimetiere — le digest et l'echelle le
								// voient). L'ignition d'une carte QUI RESTE en
								// jeu (monstre, continu) ne laisse aucune
								// trace : c'est elle que la cle doit porter.
								// Et jamais une carte DU BUT (son quota est le
								// produit, pas une ressource).
								bool is_goal = false;
								for(const auto& [gc0, gn0] : gc2) {
									(void)gn0;
									is_goal = is_goal ||
											  db.Canonical(gc0) == host;
								}
								if(is_goal)
									continue;
								uint64_t t_cont = 0;
								kt.Lookup("TYPE_CONTINUOUS", t_cont);
								const CardRow* hrow = db.Find(host);
								const bool persists =
									hrow && ((hrow->type & 0x1u) ||
											 (t_cont &&
											  (hrow->type & t_cont)));
								if(!persists)
									continue;
								quota_legacy.push_back(host);
							}
						}
						// (b) LA DERIVATION PAR LES DUAUX (s22) — le defaut.
						// Quotas : hotes a borne FINIE tires par x*, satures,
						// a dual positif, ou produisant une place que x*
						// consomme (robustesse a la degenerescence des
						// igniteurs a cout nul). Habilitants : hotes des
						// transitions TIREES par x* qui PERSISTENT en jeu —
						// Chick RESSORT par son renommage, Wolf par son
						// ignition ; personne ne les a pousses. Les hotes
						// d'un EFFECT_EXTRA_FUSION_MATERIAL s'y ajoutent tant
						// que x* tire une ignition de Fusion : l'agregat
						// DISPO (qui contient le cimetiere) n'est optimiste
						// que PAR leur concession — hypothese de STRUCTURE du
						// modele, ecrite ici, pas un nom de carte.
						std::vector<uint32_t> presence_hosts;
						std::vector<uint32_t> quota_duals =
							bm.QuotaHostsFrom(lr, &presence_hosts);
						bool fusion_pulled = false;
						for(size_t t2 = 0; t2 < bm.Transitions(); ++t2)
							fusion_pulled =
								fusion_pulled ||
								(t2 < lr.x.size() && lr.x[t2] > 1e-6 &&
								 bm.TrNames()[t2] == "igniter");
						if(fusion_pulled && k_efm)
							for(const auto& kvv : tbl.All()) {
								const uint32_t host = db.Canonical(kvv.first);
								if(!dcop.count(host))
									continue;
								for(const DeclaredEffect& g :
									kvv.second.grants)
									if(g.code_is_effect &&
									   g.code_value == k_efm)
										presence_hosts.push_back(host);
							}
						// Presence @EN JEU : seuls les hotes du deck qui
						// RESTENT en jeu portent un barreau (un sort a usage
						// unique laisse deja sa trace en zone).
						uint64_t t_cont2 = 0;
						kt.Lookup("TYPE_CONTINUOUS", t_cont2);
						std::vector<SearchConfig::SerialReq> presence_duals;
						{
							std::sort(presence_hosts.begin(),
									  presence_hosts.end());
							presence_hosts.erase(
								std::unique(presence_hosts.begin(),
											presence_hosts.end()),
								presence_hosts.end());
							for(uint32_t host : presence_hosts) {
								auto dit = dcop.find(host);
								if(dit == dcop.end())
									continue;
								const CardRow* hrow = db.Find(host);
								const bool persists2 =
									hrow && ((hrow->type & 0x1u) ||
											 (t_cont2 &&
											  (hrow->type & t_cont2)));
								if(!persists2)
									continue;
								SearchConfig::SerialReq rq;
								rq.code = host;
								rq.zone = 6;
								rq.count = (std::min)(dit->second, 3u);
								presence_duals.push_back(rq);
							}
						}
						auto finish_hosts = [](std::vector<uint32_t>& v) {
							std::sort(v.begin(), v.end());
							v.erase(std::unique(v.begin(), v.end()), v.end());
							if(v.size() > 12)
								v.resize(12);
						};
						finish_hosts(quota_legacy);
						finish_hosts(quota_duals);
						// LA VIE DES DEUX DERIVATIONS, toujours imprimee :
						// les trois derivations fausses de s21 n'ont ete
						// vues QUE par cette ligne.
						auto print_hosts = [&db](const char* tag,
												 const std::vector<uint32_t>&
													 v) {
							std::printf("  %s : ", tag);
							if(v.empty())
								std::printf("(aucun)");
							for(uint32_t qh : v)
								std::printf("%s ; ", db.Name(qh).c_str());
							std::printf("\n");
						};
						print_hosts("QUOTAS suivis (duaux)", quota_duals);
						print_hosts("QUOTAS (temoin s21, classes)",
									quota_legacy);
						{
							std::string ph;
							for(const auto& rq : presence_duals)
								ph += db.Name(rq.code) + " ; ";
							std::printf("  PRESENCE @EN JEU (x*) : %s\n",
										ph.empty() ? "(aucune)" : ph.c_str());
							std::string pl;
							for(const auto& rq : presence_legacy)
								pl += db.Name(rq.code) + " ; ";
							std::printf("  PRESENCE (temoin s21) : %s\n",
										pl.empty() ? "(aucune)" : pl.c_str());
						}
						if(opt.quota_legacy) {
							quota_hosts_wiring = quota_legacy;
							for(const auto& rq : presence_legacy)
								serial_from_balance.push_back(rq);
							std::printf("  (cablage : TEMOIN s21 — "
										"--quota-legacy)\n");
						} else {
							quota_hosts_wiring = quota_duals;
							for(const auto& rq : presence_duals)
								serial_from_balance.push_back(rq);
							std::printf("  (cablage : DUAUX s22 — temoin via "
										"--quota-legacy)\n");
						}
						std::printf("  SERIALISATION par le bilan matiere : "
									"h(depart) = %.0f, %zu sous-but(s)\n",
									h0, serial_from_balance.size());
						for(const SearchConfig::SerialReq& rq :
							serial_from_balance)
							std::printf("      x%-2u %-34s @%s\n", rq.count,
										rq.zone == 5
											? "(departs de reserve)"
											: rq.code
												  ? db.Name(rq.code).c_str()
												  : "(archetype)",
										rq.zone == 2   ? "TERRAIN"
										: rq.zone == 3 ? "CIMETIERE"
										: rq.zone == 4 ? "BANNIE"
										: rq.zone == 5 ? "RESERVE"
										: rq.zone == 6 ? "EN JEU"
													   : "DISPO");
					} else {
						// Un `h` infini ICI voudrait dire que le but est prouve
						// hors d'atteinte depuis ce deck : on le DIT, on ne
						// serialise pas sur du vide.
						std::printf("!! SERIALISATION : le bilan matiere ne rend "
									"aucun plan (h = INFINI ou solveur en "
									"defaut) — aucun sous-but pose.\n");
					}
				}
			}
		}
		// LA DECOMPOSITION A REBOURS, IMPRIMEE. C'est le juge du chantier 1, et
		// il est STRUCTUREL : sans graine, sans budget, sans tirage. Le dossier
		// ne lisait jusqu'ici que le NOMBRE de sous-produits (`snap_backward`),
		// et un nombre ne dit pas si l'operateur qu'on cherche y est.
		{
			std::vector<uint32_t> roots = target.codes;
			std::sort(roots.begin(), roots.end());
			roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
			std::vector<Requirement> reqs;
			std::vector<uint32_t> prods;
			recipe_graph.Expand(roots, 3, reqs, prods);
			std::printf("  decomposition a rebours (ordre de FABRICATION, "
						"%zu sous-produit(s)) :\n", prods.size());
			for(uint32_t p : prods)
				std::printf("      %s\n", db.Name(p).c_str());
			std::printf("  exigences rencontrees : %zu\n", reqs.size());
			for(const Requirement& q : reqs)
				if(q.kind == kReqCard)
					std::printf("      NOMMEE   %s @%s\n", db.Name(q.code).c_str(),
								q.zone ? ZoneMaskName(q.zone).c_str() : "toute zone jouable");
				else
					std::printf("      CARDINAL %s 0x%x x%u\n",
								q.kind == kReqLevel ? "niveau" : "archetype",
								q.code, q.count);
		}
	}

	// GRAPHE DE LANDMARKS APPRIS (chantier 18). Construit AVANT les workers et
	// jamais modifie ensuite : lecture seule, donc aucun verrou sur le chemin
	// chaud — c'est la difference de nature avec le graphe de recettes, qui
	// apprend pendant le run.
	static LandmarkGraph landmark_graph;
	BuildLandmarkGraph(opt, db, scripts, plan, target, landmark_graph);

	// Rejeu d'adaptation (--adapt, chantier 5bis) : le corpus entre non plus
	// en primes par coup mais en gradient sur ses propres carrefours.
	std::vector<NrpaRun> adapt_runs;
	BuildAdaptRuns(opt, db, scripts, plan, target, adapt_runs,
				   opt.recipes >= 0 ? &recipe_graph : nullptr);

	// OPTIONS (chantier 17) : le catalogue est mine UNE fois, ici, et survit a
	// toutes les phases — les workers le lisent en const. Sans corpus il n'y a
	// rien a miner : le dire plutot que laisser un mecanisme silencieusement
	// absent du chemin (la lecon de --adapt en mode reparation, 9.19 (b)).
	OptionCatalog option_catalog;
	// MINAGE EN LIGNE : `--options-online` implique un plafond de catalogue.
	// Sans cela le drapeau serait accepte et INERTE (options_n = 0 coupe tout) —
	// la famille exacte du « mecanisme silencieusement absent du chemin ».
	const uint32_t options_n =
		(opt.options_online && !opt.options_n) ? 256u : opt.options_n;
	if(options_n) {
		if(adapt_runs.empty()) {
			if(opt.options_online)
				std::printf("\n  options : aucun corpus externe (--adapt) — le run "
							"part NU et minera ses propres lignes toutes les %u s "
							"(--options-online).\n", opt.options_online);
			else
				std::printf("\n!! --options %u : aucun corpus releve (--adapt "
							"manquant ou vide) — catalogue VIDE, mecanisme eteint.\n",
							options_n);
		} else {
			option_catalog = MineOptionCatalog(adapt_runs, options_n,
											   opt.options_support,
											   opt.options_len,
											   opt.options_window,
											   opt.options_ctx);
			size_t max_len = 0, sum_len = 0;
			for(const auto& s : option_catalog.seqs) {
				max_len = (std::max)(max_len, s.size());
				sum_len += s.size();
			}
			// La taille est un RESULTAT de la selection par perte de Levin
			// (elle s'arrete quand plus rien n'ameliore), pas le parametre.
			char ctxdesc[40] = "off";
			if(opt.options_ctx >= 0)
				std::snprintf(ctxdesc, sizeof(ctxdesc),
							  "pose exact, main +/-%d", opt.options_ctx);
			std::printf("\n  options : %zu macro(s) retenue(s) sur plafond %u "
						"(support >= %u, longueur 2-%zu, moyenne %.1f, fenetre "
						"%s%u, ctx %s) ; perte modele %.1f -> %.1f log10\n",
						option_catalog.Size(), options_n,
						opt.options_support, max_len,
						option_catalog.Size()
							? double(sum_len) / double(option_catalog.Size())
							: 0.0,
						opt.options_window ? "+/-" : "",
						opt.options_window,
						ctxdesc,
						option_catalog.model_flat, option_catalog.model_opt);
		}
	}

	// --- 4. Recherche, par approfondissement progressif du nombre d'ecarts.
	SearchConfig cfg;
	ApplyMechanisms(opt, cfg);   // chantier D : UN SEUL POINT DE CABLAGE
	cfg.serial_reqs = serial_from_balance;   // 9.31 : les sous-buts calcules
	cfg.quota_hosts = quota_hosts_wiring;    // s21 : (board, quotas) en cle
	// s22, chantier 2.3 : a progres egal, le score d'archive prefere la
	// cellule aux quotas FRAIS. Fait partie du paquet « duaux » ; le temoin
	// --quota-legacy rejoue s21 a l'identique (cle sans preference).
	cfg.quota_fresh_pref = !opt.quota_legacy;
	// s22, chantier 3 : l'echelle auto-raffinante. Le modele n'est prete que
	// si la serialisation s'est armee — sans echelle, il n'y a rien a
	// raffiner, et le mecanisme s'annonce inerte via ReportMechanisms.
	cfg.refine_after = static_cast<uint32_t>(opt.refine_after);
	cfg.balance = g_balance_armed ? &g_balance_model : nullptr;
	// Le plan compte 273 etapes ; un autre deck en demandera davantage pour
	// arriver au meme endroit. On laisse de la marge, sans quoi la borne
	// couperait avant le board.
	//
	// SOUS --no-ref, le plafond ne peut PAS venir de la reference : le mode
	// promet de n'en rien tirer, et il en tirait sa borne de profondeur la plus
	// structurante (C12). Il se derive alors de la DECKLIST — au plus douze
	// decisions par carte jouable, ce qui couvre invocation, ciblage, materiaux
	// et fenetres de chaine — et il est imprime dans tous les cas, parce qu'un
	// plafond qui coupe sans se nommer produit des « ÉPUISÉ » faux (cf. C2).
	size_t deck_span = 0;
	if(!start_yrp.decks.empty()) {
		const Deck& d = start_yrp.decks[opt.target_player < static_cast<int>(
											start_yrp.decks.size())
											? opt.target_player : 0];
		deck_span = d.main.size() + d.extra.size();
	}
	const size_t from_deck = deck_span ? deck_span * 12 + 32 : 700;
	const size_t from_ref = ref_decisions * 3 / 2 + 32;
	const size_t derived = opt.no_ref ? from_deck : from_ref;
	cfg.max_decisions = opt.max_decisions ? opt.max_decisions
										  : static_cast<uint32_t>(derived);
	if(opt.max_decisions)
		std::printf("  plafond de decisions : %u (--max-decisions ; le defaut "
					"aurait ete %zu, %s)\n",
					opt.max_decisions, derived,
					opt.no_ref ? "derive de la decklist" : "derive de la reference");
	else
		std::printf("  plafond de decisions : %zu (%s)\n", derived,
					opt.no_ref
						? (deck_span ? "derive de la decklist : 12 par carte + 32"
									 : "aucune decklist lisible : defaut fixe — "
									   "poser --max-decisions")
						: "derive de la reference : 1,5x + 32");
	cfg.max_actions = 0;         // aucune borne : on cherche d'abord A atteindre
	cfg.max_nodes = 50000000;
	cfg.max_solutions = 16;
	cfg.enumeration.dedup_by_code = true;
	cfg.enumeration.max_subsets = opt.max_subsets;
	cfg.enumeration.db = &db;
	cfg.enumeration.canonical_zones = opt.canonical_zones;
	// Cible POSEE -> inclusion par defaut ; cible CAPTUREE -> egalite exacte.
	cfg.goal_subset = opt.target_subset ||
					  (!opt.target_specs.empty() && !opt.target_exact);
	cfg.plan_window = 32;
	cfg.summon_constraints = cons.summons;
	cfg.guard_after = cons.guard_after;
	cfg.guard_clauses = cons.guard;
	cfg.guard_opp_hand_release = cons.guard_opp_hand_release;
	cfg.resolve_min = cons.resolve_min;
	CheckSaturations(target.codes.size(), cons.resolve_min);
	// COMPTAGE DERIVE DU BOARD CIBLE (chantier 16, premier pas). Gratuit,
	// et il repond avant la recherche a une question qu'elle mettait des
	// minutes a ne pas repondre : de combien d'EVENEMENTS d'invocation le
	// board a besoin, et la decklist peut-elle seulement les fournir.
	if(!start_yrp.decks.empty() && !target.codes.empty()) {
		const Deck& tdeck = start_yrp.decks[
			opt.target_player < static_cast<int>(start_yrp.decks.size())
				? opt.target_player : 0];
		std::map<Mech, uint32_t> events;
		auto counts = CountTarget(target, tdeck, db, events);
		ReportTargetCounting(counts, events, db);
		if(opt.derive_summon_min)
			DeriveSummonMin(counts, cons, db, cfg);
	}
	cfg.material_req = cons.material_req;
	cfg.hint_cards = cons.hints;
	cfg.options = option_catalog.Size() ? &option_catalog : nullptr;
	// MINAGE EN LIGNE (session 14) : le corpus vivant du run. Il est declare
	// ICI pour survivre a toutes les phases, mais n'est BRANCHE que sur les
	// workers de la phase tirages — c'est la seule phase qui produit des lignes
	// completes. Le catalogue qu'il aura fini par miner est ensuite passe au
	// finisseur en STATIQUE (plus personne ne re-mine apres les tirages).
	OnlineOptions online;
	// Le catalogue de fin de tirages, tenu vivant pour le finisseur.
	std::shared_ptr<const OptionCatalog> final_online;
	if(opt.options_online) {
		online.max_options = options_n;
		online.support = opt.options_support;
		online.max_len = opt.options_len;
		online.window = opt.options_window;
		online.ctx_tol = opt.options_ctx;
		online.period_ms = opt.options_online * 1000.0;
		online.max_pool = opt.options_pool ? opt.options_pool : 1;
		online.per_worker = opt.options_per_worker ? opt.options_per_worker : 1;
		if(!adapt_runs.empty())
			online.seed = &adapt_runs;
		if(option_catalog.Size()) {
			online.cat = std::make_shared<const OptionCatalog>(option_catalog);
			online.gen = 1;   // les workers l'adoptent des le premier tour
		}
		std::printf("  options EN LIGNE : re-minage toutes les %u s, corpus "
					"vivant %zu ligne(s) max (%zu par worker)%s\n",
					opt.options_online, online.max_pool, online.per_worker,
					online.seed ? ", corpus --adapt en amorce" : "");
	}
	std::printf("  biais des indices : %.2f (%s)\n", cfg.hint_bias,
				opt.hint_bias >= 0 ? "--hint-bias" : "defaut du moteur");
	// MODE DETERMINISTE (audit 18). Imprime, parce qu'un budget qui n'est plus
	// du temps change la lecture de TOUS les compteurs de debit du rapport.
	if(opt.max_nodes)
		cfg.max_nodes = opt.max_nodes;
	if(opt.max_rollouts || opt.max_nodes) {
		std::printf("  BUDGET EN COMPTE : %llu tirage(s), %llu noeud(s) par "
					"worker\n",
					(unsigned long long)opt.max_rollouts,
					(unsigned long long)cfg.max_nodes);
		if(opt.threads != 1)
			std::printf("     !! plusieurs workers : le budget est deterministe, "
						"l'ORDRE des echanges ne l'est pas. Ajouter "
						"--threads 1 pour un run reproductible.\n");
	}
	if(opt.adapt_to_peak)
		std::printf("  gradient TRONQUE AU PIC du score (--adapt-to-peak)\n");
	// CORRECTIF (session 19) : `--elide-forced` N'ETAIT CABLE NULLE PART SAUF
	// DANS `--growth`. Trois sessions de bancs lui ont passe le drapeau sur le
	// chemin de RECHERCHE, ou il ne faisait rien. Preuve en mode deterministe :
	// `--no-elide-forced` rend « 3000 tirages, 149334 etats, 3002 adaptations »
	// a l'octet pres. C'est la TROISIEME occurrence du piege 42 (9.26 (e)) et la
	// plus chere : un drapeau qui se declare allume en etant eteint.
	//
	// Ce n'est pas un drapeau, c'est un CORRECTIF : le drapeau existait et
	// pretendait agir. Ce qui devient discutable, c'est sa VALEUR PAR DEFAUT —
	// et elle repasse a « eteint », parce que les +61 % de debit de 9.24 (k) ont
	// ete mesures sur le chemin `--growth`, jamais sur celui-ci.
	if(opt.elide_forced)
		std::printf("  coups FORCES joues en ligne (--elide-forced) — NON JUGE "
					"sur ce chemin : il y etait inerte jusqu'a la s19\n");
	// Le graphe de recettes est cree et amorce PLUS HAUT (avant le rejeu
	// d'adaptation, qui le nourrit) ; ici, seulement le cablage dans cfg.
	if(opt.recipes >= 0) {
		cfg.recipes = &recipe_graph;
	}
	// --- SESSION 17 : LES QUATRE LEVIERS ------------------------------------
	// Les chantiers 1, 3 et 4 lisent tous le graphe de recettes : sans lui ils
	// sont vivants et inertes. L'implication est appliquee PLUS HAUT (avec celle
	// de --probe-repeat) et redite ici pour chaque drapeau qui l'a declenchee.
	if(opt.op_bias > 0.0) {
		// LES DEUX FACONS DONT CE MECANISME PEUT ETRE INERTE, DITES AVANT LE
		// RUN. C'est la lecon de 9.26 (e), et elle a coute deux sessions : un
		// mecanisme eteint qui se declare allume fait mesurer deux fois le
		// temoin.
		if(opt.recipes < 0.0)
			std::printf("!! --op-bias sans --recipes : le mecanisme lit "
						"`snap_operators`, que seul l'instantane du graphe "
						"remplit. Il serait INERTE.\n");
		else if(!opt.op_recipes)
			std::printf("!! --op-bias sans --op-recipes : la decomposition n'a "
						"aucune arete d'ACQUISITION, donc aucune exigence de "
						"presence sur le TERRAIN, donc la liste sera VIDE.\n");
		else
			std::printf("  biais d'OPERATEUR : %.2f sur les coups qui jouent une "
						"carte exigee EN JEU par la decomposition a rebours\n",
						opt.op_bias);
	}
	if(opt.assign_bias > 0.0) {
		std::printf("  --assign-bias %.2f : les choix engageant un MATERIAU du "
					"graphe de recettes sont favorises\n"
					"                     (prompts de selection compris — "
					"l'identite de carte y est inconditionnelle)\n",
					opt.assign_bias);
		// LE MECANISME LIT `snap_useful`, QUI VIENT DU GRAPHE. Sans graphe il ne
		// peut rien lire et le drapeau est INERTE — il l'etait en silence
		// jusqu'a la session 18bis, tout en imprimant la ligne ci-dessus. Le
		// relevé le dit desormais, et la ligne « instantanes du graphe » du bilan
		// des tirages donne la vie du mecanisme (piege 52).
		if(!cfg.recipes)
			std::printf("!! --assign-bias SANS graphe de recettes : le "
						"mecanisme est INERTE (il lit `snap_useful`).\n"
						"   Ajouter --recipes 0 — le graphe est alors alimente "
						"et lu sans entrer dans aucun cout.\n");
	}
	// `--canonical-digest` (confondre les COLONNES dans la cle de transposition)
	// a vecu ici, avec l'avertissement automatique sur les monstres LIEN.
	// SUPPRIME (audit 18) : REFUTE sur l'etalon A — poses divisees par 2 et par
	// 12 — et la cause est NOMMEE : ce deck porte trois monstres Lien, dont les
	// FLECHES pointent des colonnes, et une zone pointee autorise une invocation
	// depuis l'extra deck. Le gain sur l'exhaustif etait reel (x1,67, +4
	// profondeurs) et il ne convertit pas.
	if(opt.assign || opt.backward) {
		if(!cfg.recipes)
			std::printf("!! --assign / --backward sans graphe de "
						"recettes : les mecanismes sont INERTES.\n");
		else
			std::printf("  session 17 : assign %s, backward %s "
						"(instantane du graphe tous les %llu tirages)\n",
						opt.assign ? "OUI" : "non",
						opt.backward ? "OUI" : "non",
						(unsigned long long)cfg.recipe_snap_period);
	}
	if(opt.hindsight > 0.0)
		std::printf("  session 17 : hindsight %.2f x alpha, au plus %zu but(s) de "
					"substitution par worker\n",
					opt.hindsight, opt.hindsight_k);
	// LE JUGE « CONVERSION OFFRE -> CHOIX » a besoin de savoir quelle carte le
	// coup retenu engage. Il n'y a plus rien a allumer : `Choice::card` est
	// renseigne INCONDITIONNELLEMENT, y compris sur les prompts de SELECTION
	// (session 18ter). Ce bloc arbitrait entre « sonde muette » et « run modifie
	// sous elle » ; l'arbitrage a disparu avec sa cause, parce que ce n'etait
	// jamais l'identite qui changeait le run mais le BIAIS D'INDICES qui s'y
	// appliquait — et celui-la est desormais garde par `IsSubsetPrompt`.
	// `--watch` : observation PURE. Aucune entree dans `cons.resolve_min`,
	// aucun `cfg.hint_cards`, aucun gradient — c'est toute la raison d'etre du
	// drapeau. Resolu par nom ou par code, comme les autres.
	for(const std::string& spec : opt.watch_specs) {
		if(cfg.probe_watch.size() >= 4) {
			std::printf("!! --watch : au plus 4 cartes (compteurs empaquetes) — "
						"« %s » ignoree\n", spec.c_str());
			continue;
		}
		uint32_t code = 0;
		if(!ResolveCard(Trimmed(spec), db, "--watch", code))
			return;
		cfg.probe_watch.push_back(db.Canonical(code));
	}
	if(!cfg.probe_watch.empty()) {
		std::printf("  --watch : %zu carte(s) OBSERVEE(S) sans contrainte ni "
					"biais —", cfg.probe_watch.size());
		for(uint32_t c : cfg.probe_watch)
			std::printf(" %s;", db.Name(c).c_str());
		std::printf("\n");
	}
	// LANDMARKS : le graphe voyage avec ses DEUX poids. Un poids sans graphe
	// serait un drapeau accepte et inerte ; un graphe sans poids est le mode
	// « appris et MESURE, n'entre pas dans le cout » — et il faut le dire, sans
	// quoi les deux se confondent dans les logs.
	if(!landmark_graph.Empty()) {
		cfg.landmarks = &landmark_graph;
		std::printf("  landmarks : ACTIFS, %zu accomplissement(s), poids tirages "
					"%.2f, poids finisseur %.2f%s\n",
					landmark_graph.Items().size(), opt.landmark_weight,
					opt.landmark_h,
					(opt.landmark_weight == 0.0 && opt.landmark_h == 0.0)
						? "  (appris et MESURES, n'entrent dans aucun cout)"
						: "");
	} else if(opt.landmark_weight > 0.0 || opt.landmark_h > 0.0) {
		std::printf("!! --landmark-w/--landmark-h sans graphe de landmarks : "
					"le poids est INERTE (il manque --landmarks)\n");
	}
	// Les pointeurs sont branches : le rapport peut dire ce qui est ACTIF et ce
	// qui est demande mais INERTE. Lu AVANT le premier tirage, il rend un bras
	// de mesure jetable avant de depenser le budget, pas apres.
	ReportMechanisms(cfg, "transplant");
	// Optimisation de cout anytime : la recherche continue apres la premiere
	// solution (chaque solution resserre la borne), l'ensemble par worker est
	// borne par remplacement du pire, le score de but NRPA est lexicographique.
	if(opt.optimize) {
		cfg.anytime = true;
		cfg.max_solutions = 24;
		std::printf("\n  OPTIMISATION anytime : cout lexicographique (brulees, "
					"actions, decisions),\n  la reference coute %u/%u/%zu — la "
					"borne a battre.%s\n", ref_burned, ref_actions, ref_decisions,
					opt.burn_limit
						? "  (borne brulees ensemencee)" : "");
	}
	// Les cartes a resoudre (--resolve) recoivent D'OFFICE le biais des
	// indices : la ligne DOIT les engager, et la mesure (session 4) est sans
	// appel — la politique ne rippe JAMAIS sans coup de pouce, malgre le
	// gradient de +100 par resolution.
	for(const ResolveReq& req : cons.resolve_min)
		if(std::find(cfg.hint_cards.begin(), cfg.hint_cards.end(), req.code) ==
		   cfg.hint_cards.end())
			cfg.hint_cards.push_back(req.code);
	if(!cons.no_activate.empty())
		cfg.enumeration.no_activate = &cons.no_activate;
	if(!cons.no_chain.empty())
		cfg.enumeration.no_chain = &cons.no_chain;
	if(!cons.self_negate.empty())
		cfg.self_negate = &cons.self_negate;   // s22ter : discipline choisie
	cfg.enumeration.mp1_only = opt.mp1_only;   // s22quater : combo en MP1 seule
	// Verdict informatif : ici la reference joue sur un AUTRE deck, sa
	// conformite ne conditionne aucun invariant — mais elle dit si le plan
	// servi en repertoire respecte lui-meme la contrainte demandee.
	ReportConstraints(cons, ref, db);

	// Faisabilite des --resolve : la carte a resoudre doit EXISTER dans le
	// deck de depart (main + extra) — sinon la contrainte est insatisfiable
	// et AUCUNE ligne n'existe, quelle que soit la recherche. Le minimum peut
	// en revanche depasser le nombre de copies : une carte se recupere
	// (arbitrage du joueur : Omega revient de la zone bannie via Dis Pater).
	if(!cons.resolve_min.empty()) {
		const Deck& deck = start_yrp.decks[opt.target_player];
		std::map<uint32_t, uint32_t> avail;
		for(const auto* list : { &deck.main, &deck.extra })
			for(uint32_t c : *list)
				++avail[db.Canonical(c)];
		bool impossible = false;
		std::printf("\n--- faisabilite des resolutions exigees ---\n");
		for(const ResolveReq& req : cons.resolve_min) {
			uint32_t have = avail.count(req.code) ? avail[req.code] : 0;
			std::printf("  %-40s x%u exigee(s), %u copie(s) au deck%s\n",
						db.Name(req.code).c_str(), req.min_count, have,
						have ? "" : "   <-- ABSENTE");
			if(!have)
				impossible = true;
		}
		if(impossible) {
			std::printf("\n  Une carte a resoudre n'existe pas dans ce deck : la "
						"contrainte est\n  INSATISFIABLE — aucune ligne n'existe. "
						"Recherche annulee.\n");
			return;
		}
	}

	unsigned threads = opt.threads ? opt.threads
								   : (std::max)(1u, std::thread::hardware_concurrency());
	std::vector<Solution> sols;
	uint32_t best_overlap = 0, best_monsters = 0;
	std::vector<uint32_t> best_board;
	// Chemin menant au meilleur etat rencontre, toutes passes confondues :
	// l'entree du finisseur. Le detail du terrain dit ce qui differe quand
	// tous les codes y sont.
	std::vector<std::vector<uint8_t>> best_path;
	std::vector<QueriedCard> best_mzone, best_szone;
	// La meilleure ligne JOINTE (s23) : max lexicographique (rips, board),
	// toutes passes confondues. Ecrite en fin de run (best_joint_*.yrp) pour
	// etre reinjectee par --approach — les lignes a rips complets mouraient
	// avec le run (47 tirages a 3 rips du run s23_USER_B, aucun conserve).
	uint32_t best_joint_rp = 0, best_joint_overlap = 0;
	std::vector<std::vector<uint8_t>> best_joint_path;
	auto merge_joint = [&](const SearchStats& st,
						   const std::vector<std::vector<uint8_t>>& pre) {
		const size_t cand_len = pre.size() + st.best_joint_path.size();
		if(st.best_joint_rp &&
		   (st.best_joint_rp > best_joint_rp ||
			(st.best_joint_rp == best_joint_rp &&
			 (st.best_joint_overlap > best_joint_overlap ||
			  (st.best_joint_overlap == best_joint_overlap &&
			   !best_joint_path.empty() &&
			   cand_len < best_joint_path.size()))))) {
			best_joint_rp = st.best_joint_rp;
			best_joint_overlap = st.best_joint_overlap;
			best_joint_path = pre;
			best_joint_path.insert(best_joint_path.end(),
								   st.best_joint_path.begin(),
								   st.best_joint_path.end());
		}
	};
	// Un chemin enracine sur une APPROCHE n'est start-roote que si l'approche
	// partage le GABARIT du depart — vrai pour les best_joint/best_approach
	// ecrits par ce pipeline (WriteYrp1 copie graine et parametres), VERIFIE
	// et non suppose : l'it3 de la s23 a perdu sa meilleure ligne jointe
	// (3r_3of6, workers d'approche) parce que l'exclusion etait aveugle.
	auto same_gabarit = [&](const Replay* src) {
		return src &&
			   std::equal(std::begin(src->seed), std::end(src->seed),
						  std::begin(start_yrp.seed)) &&
			   src->duel_flags == start_yrp.duel_flags &&
			   src->start_lp == start_yrp.start_lp &&
			   src->start_hand == start_yrp.start_hand &&
			   src->draw_count == start_yrp.draw_count;
	};
	// Budget GLOBAL : les trois passes se partagent solve_ms, elles ne
	// l'empilent pas — un --solve-ms de 600 s doit durer ~600 s.
	double spent = 0;

	// Archive Go-Explore GLOBALE (fusion des archives des passes, une entree
	// par cellule = board complet) et politique NRPA fusionnee (moyenne des
	// poids des workers) : la matiere premiere du finisseur. La politique
	// mourait avec le run alors qu'elle est exactement le guide qu'il faut a
	// la conversion — c'est le verrou mesure trois fois (finisseur epuise a
	// ~6 etats depuis le seul meilleur etat).
	std::unordered_map<uint64_t, ArchiveEntry> global_archive;
	NrpaPolicy merged_policy;
	unsigned policy_workers = 0;
	// L'HERITAGE DU ROUND PRECEDENT (s24, --carry). Adopte AVANT toute phase :
	// le finisseur de ce round lira ces cellules dans ses racines, et les
	// workers de tirages en seront semes plus bas. La vie est dite — une
	// archive portee en silence serait indiscernable d'une archive vide.
	if(carry && opt.carry &&
	   (!carry->archive.empty() || !carry->policy.empty())) {
		global_archive = std::move(carry->archive);
		merged_policy = std::move(carry->policy);
		policy_workers = carry->policy_workers;
		std::printf("  ARCHIVE PORTEE (s24, --carry) : %zu cellule(s), "
					"politique %zu poids (%u worker(s)) reprises du round "
					"precedent\n",
					global_archive.size(), merged_policy.size(),
					policy_workers);
	}
	// L'INSTANTANE DE SEMIS (s24, --carry) : les cellules HERITEES, figees
	// AVANT que les phases de ce round n'ecrivent — la sonde et les workers
	// de tirages sont semes du round d'HIER, pas du bruit d'aujourd'hui.
	// Chemins start-rootes par construction (meme gabarit entre rounds) ;
	// ne JAMAIS semer un finisseur enracine sur un prefixe.
	std::vector<ArchiveEntry> carry_seed;
	if(opt.carry)
		for(const auto& [cell_, e_] : global_archive)
			carry_seed.push_back(e_);
	// Borne brulees PARTAGEE entre workers (session 6) : semee par
	// --burn-limit, resserree par chaque amelioration de chaque phase — un
	// worker qui trouve 19 coupe chez les quinze autres des la decision
	// suivante. --no-burn-share la debranche (A/B).
	std::atomic<uint32_t> shared_burn{ opt.burn_limit ? opt.burn_limit
													  : UINT32_MAX };
	auto merge_archive = [&](const std::vector<ArchiveEntry>& a) {
		for(const ArchiveEntry& e : a) {
			auto [it, fresh] = global_archive.try_emplace(e.cell, e);
			if(!fresh && e.score > it->second.score)
				it->second = e;
		}
	};
	// GO-EXPLORE COMPLET, la fusion RE-ENRACINEE (s24, --archive-fin). Les
	// archives des recherches du finisseur sont relatives a LEUR racine (le
	// prefixe est rejoue avant la construction du Search) : on re-enracine —
	// chemin = prefixe + chemin, decisions cumulees, et la queue de
	// profondeur du score re-etalonnee EXACTEMENT (bits bas = ~profondeur ;
	// les composantes d'ETAT — progres, rips, overlap, brulees — se lisent du
	// duel et restent justes). Deux approximations CONNUES et SURES : la cle
	// de cellule et le nibble « quotas frais » datent de la racine du
	// finisseur (les usages du prefixe leur manquent) — au pire des cellules
	// EN PLUS (sur-partitionnement, direction sure de s21), jamais un
	// representant corrompu ; une re-observation par un worker semé reprend
	// la cle vivante. APPELER SOUS fmx (global_archive n'est pas protegee).
	size_t fin_cells_new = 0, fin_cells_upd = 0;
	auto merge_rebased = [&](const std::vector<ArchiveEntry>& a,
							 const std::vector<std::vector<uint8_t>>& pre) {
		for(ArchiveEntry e : a) {
			if(e.path.empty())
				continue;
			e.decisions += static_cast<uint32_t>(pre.size());
			std::vector<std::vector<uint8_t>> full = pre;
			full.insert(full.end(), e.path.begin(), e.path.end());
			e.path = std::move(full);
			const uint64_t tail =
				0xFFFFFFFFull -
				(std::min<uint64_t>)(e.decisions, 0xFFFFFF00ull);
			if(cfg.serial_reqs.empty())
				e.score = (e.score & ~0xFFFFFFFFull) | tail;
			else
				e.score = (e.score & ~0xFFFFFFull) | (tail >> 8);
			auto [it, fresh] = global_archive.try_emplace(e.cell, e);
			if(fresh)
				++fin_cells_new;
			else if(e.score > it->second.score) {
				it->second = std::move(e);
				++fin_cells_upd;
			}
		}
	};

	// --- 4a. Sonde gloutonne. La recherche a ecarts bornes ne descend qu'aussi
	// profond que son budget d'ecarts ; quand le plan ne s'applique pas des
	// l'ouverture, cela plafonne a une dizaine de decisions alors que le board
	// en demande des centaines. La descente guidee, elle, va au fond : elle dit
	// jusqu'ou ce deck sait aller, ce qu'aucun echec de LDS ne revele.
	{
		std::printf("\n--- sonde : jusqu'ou ce deck va-t-il depuis cette main ? ---\n");
		auto t0 = Clock::now();
		// Thread dedie, pour la meme raison que OpeningHand : une arene ne
		// s'initialise jamais sur un thread qui en possede deja une.
		std::thread([&] {
		Arena probe_arena;
		std::string err;
		if(probe_arena.Init(opt.arena_mb << 20, 0, err)) {
			{
				Duel probe(db, scripts, &probe_arena);
				if(probe.Create(start_yrp.seed, start_yrp.duel_flags,
								start_yrp.start_lp, start_yrp.start_hand,
								start_yrp.draw_count, err) &&
				   probe.Setup(start_yrp, err,
							   cons.opp_hand.empty() ? nullptr : &cons.opp_hand,
							   static_cast<uint8_t>(1 - opt.target_player))) {
					if(opt.stop_gc)
						probe.SetLuaGc(false);
					SearchConfig pcfg = cfg;
					pcfg.time_limit_ms = (std::min)(opt.solve_ms / 6.0, 20000.0);
					pcfg.archive_k = opt.archive_k;
					Search s(probe, probe_arena, start_yrp, pcfg);
					// Semis du round precedent (s24, --carry) : la sonde
					// repart de la frontiere d'hier.
					if(!carry_seed.empty())
						std::printf("  semis d'archive (s24, --carry) : %zu "
									"cellule(s) -> sonde\n",
									s.SeedArchive(carry_seed));
					s.RunGuided(target);
					merge_archive(s.Archive());
					const SearchStats& st = s.Stats();
					std::printf("  %llu etats, %.1f s : au mieux %u des %zu cartes "
								"cibles, %u monstre(s)\n",
								(unsigned long long)st.nodes, st.ms / 1000.0,
								st.best_overlap, target.codes.size(),
								st.best_monsters);
					if(st.best_overlap > best_overlap) {
						best_board = st.best_board;
						best_path = st.best_path;
						best_mzone = st.best_mzone;
						best_szone = st.best_szone;
					}
					best_overlap = (std::max)(best_overlap, st.best_overlap);
					best_monsters = (std::max)(best_monsters, st.best_monsters);
					merge_joint(st, {});
					for(const auto& x : s.Solutions())
						sols.push_back(x);
				} else {
					std::printf("  !! duel de depart non initialisable : %s\n",
								err.c_str());
				}
			}
			probe_arena.Shutdown();
		}
		}).join();
		spent += MsSince(t0);
		prof::PrintPhase("sonde");
	}

	// --- 4b. Tirages profonds. C'est la passe qui a une chance d'aller au bout :
	// elle descend jusqu'a la fin du tour a chaque essai, la ou les deux autres
	// s'arretent a quelques dizaines de decisions. Deux moteurs se partagent
	// les workers :
	//   - tirages GLOUTONS purs : evaluation des fils, forts localement ;
	//   - tirages NRPA : politique apprise par code de coup (plan_key), le
	//     repertoire en biais, SANS evaluation des fils — chaque decision coute
	//     plusieurs fois moins cher, et la politique concentre les tirages.
	{
		std::printf("\n--- tirages profonds guides par le repertoire ---\n");
		// PARTAGE DU BUDGET ENTRE PHASES. Sept constantes au jugé decident du
		// volume relatif des trois passes dont le §9.11 compare les rendements,
		// et aucune n'etait imprimee (2.6/C17) : deux runs de meme --solve-ms
		// pouvaient donner des volumes tres differents sans qu'un mot le dise.
		double budget = (std::max)(0.0, (opt.solve_ms - spent) * 0.7);
		// Budget reserve au finisseur (--finisher-min) : les tirages cedent
		// la place quand la conversion est la question.
		if(opt.finisher_min > 0)
			budget = (std::max)(0.0, (std::min)(
				budget, opt.solve_ms - spent - opt.finisher_min));
		std::mutex merge;
		struct ModeStats {
			uint64_t nodes = 0, rollouts = 0, cuts = 0, turn_cuts = 0, adapts = 0;
			// Profil de progression par tirage (s21) : additif entre workers.
			uint64_t sp_final[72] = {};
			uint64_t sp_at_sum = 0, sp_lines = 0;
			// Vie du retour au barreau (--reenter, s21) : additif.
			uint64_t reenter_rollouts = 0, reenter_fail = 0,
					 reenter_base_sum = 0;
			// Vie du raffinement (s22, chantier 3) : workers raffines, sous-
			// barreaux, etats archives au-dela de la porte.
			uint64_t refine_done = 0, refine_subrungs = 0, refine_top_hits = 0;
			uint64_t hint_seen = 0, hint_taken = 0;
			// Ventilation (audit 18) : `sel` = prompts de SOUS-ENSEMBLE, ou
			// l'identite de carte est approximative et n'existe que sous
			// `--card-on-select` ; `exact` = le reste, ou elle designe vraiment
			// le coup. Sans cette separation le compteur avait trois causes.
			uint64_t hint_exact = 0, hint_sel = 0;
			// Vie de --adapt-to-peak, et travail refait par la transposition.
			uint64_t peak_trunc = 0, tt_reexplored = 0;
			uint64_t rr[4] = { 0, 0, 0, 0 };
			uint64_t burn_cuts = 0, goal_hits = 0;
			// LES CONTRAINTES COUPENT ICI, dans les tirages — pas dans les
			// passes LDS ou `PrintCuts` les affichait deja a zero. Les brancher
			// sur la ligne de phase des tirages etait le point 1.1/1.2 de
			// l'audit, et c'est la seule facon de repondre a la question
			// laissee ouverte par le §9.11 : la garde elague-t-elle utilement,
			// ou rase-t-elle l'espace ?
			uint64_t constraint_cuts = 0, guard_cuts = 0, self_negate_cuts = 0;
			// Options (chantier 17) : prises / decisions absorbees / avortees.
			uint64_t macro_taken = 0, macro_absorbed = 0, macro_aborted = 0;
			size_t ctx_entries = 0;
			bool ctx_capped = false;
			uint32_t overlap = 0, monsters = 0, overlap_ripped = 0;
			// BANDIT DE TETE (--qhat) : la vie du mecanisme, agregee.
			uint64_t qhat_decisions = 0, qhat_first = 0, qhat_playouts = 0;
			uint64_t qhat_fallback = 0;
			double qhat_reward_sum = 0;
			size_t qhat_nodes = 0, qhat_codes = 0, qhat_bytes = 0;
			// LANDMARKS : la vie du mecanisme LA OU IL AGIT. Le compteur
			// existait deja, mais il n'etait imprime que par `PrintCuts`, qui
			// ne couvre pas la phase tirages — donc precisement pas la phase ou
			// le poids `--landmark-w` travaille. Piege 52 sur notre propre
			// mecanisme, trouve en relisant un A/B ou la colonne etait vide.
			double lm_h_sum = 0.0;
			uint64_t lm_h_count = 0;
			// SESSION 17 : la vie des quatre mecanismes. Meme raison que
			// ci-dessus, et la lecon est fraiche — l'A/B des landmarks est parti
			// sans savoir si le `h` decroissait.
			double rec_roll_sum = 0.0, rec_roll_d0_sum = 0.0;
			uint64_t rec_roll_count = 0;
			double backward_sum = 0.0;
			uint64_t backward_count = 0;
			uint64_t hindsight_goals = 0, hindsight_adapts = 0;
			uint64_t hindsight_quota_spent = 0, hindsight_quota_fresh = 0;
			uint64_t recipe_snaps = 0, snap_products = 0, snap_useful = 0,
					 snap_backward = 0;
		// Vie du biais d'operateur (--op-bias, chantier 2 de la session 19).
		uint64_t op_offered = 0, op_taken = 0, op_listed = 0;
			// SONDE DE REPETITION (--probe-repeat). Tout y est ADDITIF entre
			// workers sauf min/max et la reference d0 — qui est la meme pour
			// tous (meme etat de depart), donc n'importe laquelle vaut.
			RepeatProbe rep[4];
			void AddRepeat(const SearchStats& s) {
				for(int i = 0; i < 4; ++i) {
					const RepeatProbe& r = s.rep[i];
					RepeatProbe& a = rep[i];
					if(!a.code)
						a.code = r.code;
					for(int k = 0; k < 5; ++k)
						a.reached[k] += r.reached[k];
					a.more_n += r.more_n;
					a.more_sum += r.more_sum;
					a.rest_sum += r.rest_sum;
					a.more_kept += r.more_kept;
					a.more_lost += r.more_lost;
					a.after_sum += r.after_sum;
					a.first_depth_sum += r.first_depth_sum;
					a.d0_samples += r.d0_samples;
					a.known |= r.known;
					a.offer_steps += r.offer_steps;
					a.offer_rollouts += r.offer_rollouts;
					a.offer_msgs |= r.offer_msgs;
					a.act_rollouts += r.act_rollouts;
					a.act_total += r.act_total;
					for(int z = 0; z < 6; ++z)
						a.zone_rollouts[z] += r.zone_rollouts[z];
					a.taken_steps += r.taken_steps;
					a.yn_steps += r.yn_steps;
					a.yn_yes += r.yn_yes;
					for(int k = 0; k < 6; ++k)
						a.offer_by[k] += r.offer_by[k];
					if(r.more_n) {
						a.more_min = (std::min)(a.more_min, r.more_min);
						a.more_max = (std::max)(a.more_max, r.more_max);
					}
					// La CLASSIFICATION conserve/consomme est faite DANS le
					// worker, contre sa reference contemporaine — c'est la seule
					// comparaison valide. Le d0 agrege n'est qu'informatif : on
					// en garde le plus grand, qui est le plus recent au sens du
					// graphe (il ne fait que s'enrichir).
					if(r.d0 != 0xffffffffu &&
					   (a.d0 == 0xffffffffu || r.d0 > a.d0)) {
						a.d0 = r.d0;
						a.rest0 = r.rest0;
					}
				}
			}
		};
		ModeStats greedy, nrpa;
		// SONDE DE LA PREMIERE DECISION, agregee entre workers : c'est
		// l'instrument que le chantier exige AVANT tout A/B — n^ et Q^ par
		// ouverture. Les effectifs et les sommes de recompense sont additifs,
		// donc l'agregation est exacte et non une moyenne de moyennes.
		std::map<uint64_t, BanditProbe> qhat_root;
		// Graine derivee du temps par defaut, et IMPRIMEE : l'ancienne
		// constante faisait de chaque relance le meme run (mesure : 8/8 sur
		// une graine, 7/8 sur trois autres — relancer doit re-tirer).
		uint64_t base_seed = opt.seed;
		if(!base_seed) {
			base_seed = static_cast<uint64_t>(
				std::chrono::high_resolution_clock::now().time_since_epoch().count());
			base_seed ^= base_seed >> 33;
			base_seed *= 0xff51afd7ed558ccdull;
			base_seed ^= base_seed >> 33;
			if(!base_seed)
				base_seed = 1;
		}
		std::printf("  graine : %llu  (--seed %llu pour rejouer)\n",
					(unsigned long long)base_seed, (unsigned long long)base_seed);
		{
			const int lvl = opt.nrpa_level > 0 ? opt.nrpa_level
											  : ((budget > 180000.0) ? 3 : 2);
			// Le nombre de tirages par appel de niveau est iters^L : il etait
			// imprime en dur (576/13824), ce qui aurait menti des que
			// --nrpa-iters bouge — exactement la variable cachee de C15.
			const uint32_t it = opt.nrpa_iters ? opt.nrpa_iters
											   : SearchConfig{}.nrpa_iters;
			double rollouts = 1;
			for(int k = 0; k < lvl; ++k)
				rollouts *= it;
			std::printf("  niveau NRPA : %d  (%s ; ~%.0f tirages par appel de "
						"niveau, iters %u)\n", lvl,
						opt.nrpa_level > 0 ? "--nrpa-level"
										   : "defaut, seuil de 180 s sur le "
											 "budget des tirages",
						rollouts, it);
			// Le cadran d'adaptation, imprime des qu'il quitte le defaut : un
			// reglage qui change l'algorithme sans se nommer est une variable
			// cachee (C15).
			// (`--nrpa-lr`, les repetitions limitees de GNRPA-LR, etait imprime
			// ici. SUPPRIME — audit 18, REFUTE en 9.19. La sortie de niveau se
			// fait desormais sur la seule stagnation.)
			if(opt.nrpa_alpha > 0)
				std::printf("  adaptation  : alpha %.3f (%s), sortie de niveau "
							"sur stagnation seule\n",
							opt.nrpa_alpha,
							"--nrpa-alpha");
		}
		// Meilleure sequence GLOBALE, partagee entre les workers NRPA : les
		// redemarrages repartent de la meilleure ligne connue de tous au lieu
		// de reapprendre les memes sous-lignes chacun dans son coin.
		NrpaShared shared_best;
		auto t0 = Clock::now();
		// Premiere echeance de minage en ligne : une periode apres le depart —
		// avant, le corpus vivant n'a encore rien de complet a offrir.
		online.next = std::chrono::steady_clock::now() +
					  std::chrono::milliseconds(
						  static_cast<long long>(online.period_ms));

		auto worker = [&](unsigned id) {
			// Sept workers sur huit en NRPA. Le quart glouton d'origine a ete
			// re-mesure sur les runs disciplines de la session 4 : crete 2/8
			// pour ~6 M etats, trois runs sur trois, pendant que NRPA fait
			// 7-8/8 — on lui laisse une part residuelle (exploration autre),
			// plus le quart.
			const bool use_nrpa = opt.nrpa && (id % 8 != 1);
			Arena la;
			std::string err;
			if(!la.Init(opt.arena_mb << 20, 0, err))
				return WorkerAbort("arene (tirages NRPA)", err);
			{
				Duel local(db, scripts, &la);
				if(local.Create(start_yrp.seed, start_yrp.duel_flags,
								start_yrp.start_lp, start_yrp.start_hand,
								start_yrp.draw_count, err) &&
				   local.Setup(start_yrp, err,
							   cons.opp_hand.empty() ? nullptr : &cons.opp_hand,
							   static_cast<uint8_t>(1 - opt.target_player))) {
					if(opt.stop_gc)
						local.SetLuaGc(false);
					SearchConfig wcfg = cfg;
					wcfg.time_limit_ms = budget;
					wcfg.novelty_patience = patience;
					// Niveau d'imbrication NRPA. Ce n'est PAS un reglage fin : il
					// change le cout d'un appel de niveau de iters^2 (~576
					// tirages) a iters^3 (~13 824), c'est-a-dire l'algorithme
					// d'echantillonnage lui-meme. Le seuil de 180 s qui le
					// choisissait tout seul tombait exactement sur la ligne de
					// partage des commandes comparees aux sessions 5-7 — un run
					// de 600 s sans --finisher-min laisse 420 s aux tirages
					// (niveau 3), le MEME run avec --finisher-min 420000 en
					// laisse ~180 (niveau 2) : une variable cachee dans
					// plusieurs A/B publies (C15). Il est desormais explicite,
					// et sa valeur est imprimee ci-dessous.
					wcfg.nrpa_level = opt.nrpa_level > 0
										  ? opt.nrpa_level
										  : ((budget > 180000.0) ? 3 : 2);
					if(opt.nrpa_bias >= 0)
						wcfg.nrpa_bias_known = static_cast<float>(opt.nrpa_bias);
					wcfg.nrpa_restart_keep = static_cast<float>(opt.nrpa_keep);
					wcfg.nrpa_shared = &shared_best;
					// Recette Montparnasse (chantier 2) : pas d'adaptation et
					// iterations par niveau, jusqu'ici en dur. 0 = defaut du
					// moteur, a l'octet pres.
					if(opt.nrpa_alpha > 0)
						wcfg.nrpa_alpha = static_cast<float>(opt.nrpa_alpha);
					if(opt.nrpa_iters)
						wcfg.nrpa_iters = opt.nrpa_iters;
					// Minage EN LIGNE : ce worker verse ses meilleures lignes au
					// corpus vivant et rachete le catalogue entre deux iterations
					// de niveau superieur. `worker_id` sert au quota par worker
					// (la pompe a diversite).
					if(opt.options_online) {
						wcfg.options_online = &online;
						wcfg.worker_id = id;
					}
					wcfg.archive_k = opt.archive_k;
					// Borne brulees partagee entre workers (session 6).
					if(opt.optimize && opt.burn_share)
						wcfg.shared_burn = &shared_burn;
					// Prior par rejeu : la politique demarre en sachant
					// ripper (attenuee ensuite comme les poids appris).
					if(!prior_policy.empty())
						wcfg.nrpa_init = &prior_policy;
					// Rejeu d'adaptation : meme injection, gradient au lieu
					// de prime (chantier 5bis).
					if(!adapt_runs.empty()) {
						wcfg.nrpa_adapt_runs = &adapt_runs;
						wcfg.nrpa_adapt_passes = opt.adapt_passes;
					}
					// Politique a deux niveaux (chantier 5ter).
					wcfg.ctx_shrink = static_cast<float>(opt.ctx_shrink);
					// Conditionnement par le chemin (MCPS) et plafond de la
					// table contextuelle : les deux doivent voyager ENSEMBLE,
					// sinon le contexte est calcule et jamais borne.
					// BANDIT DE TETE (--qhat) : les quatre cadrans voyagent
					// ensemble ; sans la fenetre ni le plafond, la profondeur
					// seule ferait un mecanisme non borne.
					wcfg.qhat_depth = opt.qhat_depth;
					wcfg.qhat_window = opt.qhat_window;
					wcfg.qhat_rho = opt.qhat_rho;
					wcfg.qhat_max_nodes = opt.qhat_nodes;
					wcfg.ctx_max = opt.ctx_max;
					wcfg.nrpa_temp = static_cast<float>(opt.nrpa_temp);
					Search s(local, la, start_yrp, wcfg);
					// Semis du round precedent (s24, --carry) : chaque worker
					// de tirages repart de la frontiere d'hier — le retour au
					// barreau re-entre des cellules qu'aucun tirage de CE
					// round n'a encore atteintes. Vie dite une fois (worker 0).
					if(!carry_seed.empty()) {
						const size_t sown = s.SeedArchive(carry_seed);
						if(id == 0)
							std::printf("  semis d'archive (s24, --carry) : "
										"%zu cellule(s) par worker\n", sown);
					}
					// Graine distincte par worker : sans cela les seize tirent
					// exactement la meme sequence de lignes.
					uint64_t seed = base_seed + id * 0x100000001b3ull;
					if(use_nrpa)
						s.RunNrpa(target, plan, seed);
					else
						s.RunRollouts(target, plan, 1000000, seed);
					std::lock_guard<std::mutex> lock(merge);
					for(const auto& x : s.Solutions())
						sols.push_back(x);
					merge_archive(s.Archive());
					if(use_nrpa && !s.LearnedPolicy().empty()) {
						for(const auto& [k2, w] : s.LearnedPolicy())
							merged_policy[k2] += w;
						++policy_workers;
					}
					ModeStats& m = use_nrpa ? nrpa : greedy;
					m.nodes += s.Stats().nodes;
					m.rollouts += s.Stats().rollout_count;
					m.cuts += s.Stats().novelty_cuts;
					m.turn_cuts += s.Stats().turn_cuts;
					for(int k = 0; k < 72; ++k)
						m.sp_final[k] += s.Stats().sp_final[k];
					m.sp_at_sum += s.Stats().sp_at_sum;
					m.sp_lines += s.Stats().sp_lines;
					m.reenter_rollouts += s.Stats().reenter_rollouts;
					m.reenter_fail += s.Stats().reenter_fail;
					m.reenter_base_sum += s.Stats().reenter_base_sum;
					m.refine_done += s.Stats().refine_done;
					m.refine_subrungs += s.Stats().refine_subrungs;
					m.refine_top_hits += s.Stats().refine_top_hits;
					m.constraint_cuts += s.Stats().constraint_cuts;
					m.guard_cuts += s.Stats().guard_cuts;
					m.self_negate_cuts += s.Stats().self_negate_cuts;
					m.adapts += s.Stats().nrpa_adapts;
					m.ctx_entries = (std::max)(m.ctx_entries,
											   s.Stats().ctx_entries);
					m.ctx_capped |= s.Stats().ctx_capped;
					m.macro_taken += s.Stats().macro_taken;
					m.macro_absorbed += s.Stats().macro_absorbed;
					m.macro_aborted += s.Stats().macro_aborted;
					m.qhat_decisions += s.Stats().qhat_decisions;
					m.qhat_first += s.Stats().qhat_first;
					m.qhat_playouts += s.Stats().qhat_playouts;
					m.qhat_fallback += s.Stats().qhat_fallback;
					m.qhat_reward_sum += s.Stats().qhat_reward_sum;
					m.qhat_nodes = (std::max)(m.qhat_nodes,
											  s.Stats().qhat_nodes);
					m.qhat_codes = (std::max)(m.qhat_codes,
											  s.Stats().qhat_codes);
					m.qhat_bytes += s.Stats().qhat_bytes;
					for(const BanditProbe& b : s.RootBandit()) {
						BanditProbe& agg = qhat_root[b.key];
						agg.key = b.key;
						if(!agg.code)
							agg.code = b.code;
						agg.n += b.n;
						agg.w += b.w;
						agg.nhat += b.nhat;
						agg.qhat_sum += b.qhat_sum;
					}
					m.hint_seen += s.Stats().hint_seen;
					m.hint_taken += s.Stats().hint_taken;
					m.hint_exact += s.Stats().hint_seen_exact;
					m.hint_sel += s.Stats().hint_seen_sel;
					m.peak_trunc += s.Stats().peak_truncations;
					m.tt_reexplored += s.Stats().tt_reexplored;
					m.AddRepeat(s.Stats());
					m.lm_h_sum += s.Stats().landmark_h_sum;
					m.lm_h_count += s.Stats().landmark_h_count;
					// SESSION 17 : la vie des quatre mecanismes (piege 52).
					m.rec_roll_sum += s.Stats().rec_roll_sum;
					m.rec_roll_d0_sum += s.Stats().rec_roll_d0_sum;
					m.rec_roll_count += s.Stats().rec_roll_count;
					m.backward_sum += s.Stats().backward_sum;
					m.backward_count += s.Stats().backward_count;
					m.hindsight_goals += s.Stats().hindsight_goals;
					m.hindsight_quota_spent += s.Stats().hindsight_quota_spent;
					m.hindsight_quota_fresh += s.Stats().hindsight_quota_fresh;
					m.hindsight_adapts += s.Stats().hindsight_adapts;
					m.recipe_snaps += s.Stats().recipe_snaps;
					// Instantanes : la TAILLE est la meme pour tous les workers
					// (meme graphe, meme cible), on garde donc la plus grande —
					// un worker qui n'a pas encore rafraichi rendrait zero.
					m.snap_products = (std::max)(m.snap_products,
												 s.Stats().snap_products);
					m.snap_useful = (std::max)(m.snap_useful,
											   s.Stats().snap_useful);
					m.snap_backward = (std::max)(m.snap_backward,
												 s.Stats().snap_backward);
					// La TAILLE de la liste est la meme pour tous (meme graphe) ;
					// les EMPLOIS, eux, s'additionnent — ce sont des decisions.
					m.op_listed = (std::max)(m.op_listed,
											 s.Stats().op_bias_listed);
					m.op_offered += s.Stats().op_bias_offered;
					m.op_taken += s.Stats().op_bias_taken;
					for(int k = 0; k < 4; ++k)
						m.rr[k] += s.Stats().resolve_reached[k];
					m.burn_cuts += s.Stats().burn_cuts;
					m.goal_hits += s.Stats().goal_hits;
					m.overlap_ripped = (std::max)(m.overlap_ripped,
												  s.Stats().best_overlap_ripped);
					m.overlap = (std::max)(m.overlap, s.Stats().best_overlap);
					m.monsters = (std::max)(m.monsters, s.Stats().best_monsters);
					if(s.Stats().best_overlap > best_overlap) {
						best_board = s.Stats().best_board;
						best_path = s.Stats().best_path;
						best_mzone = s.Stats().best_mzone;
						best_szone = s.Stats().best_szone;
					}
					best_overlap = (std::max)(best_overlap, s.Stats().best_overlap);
					best_monsters = (std::max)(best_monsters, s.Stats().best_monsters);
					merge_joint(s.Stats(), {});
				} else {
					WorkerAbort("duel (tirages NRPA)", err);
				}
			}
			ReportPoison("tirages NRPA", la);
			la.Shutdown();
		};

		std::vector<std::thread> pool;
		for(unsigned i = 0; i < threads; ++i)
			pool.emplace_back(worker, i);
		for(auto& t : pool)
			t.join();
		// Moyenne des poids : les politiques des workers sont des logits
		// additifs de meme echelle, leur moyenne est la fusion standard.
		if(policy_workers > 1)
			for(auto& [k2, w] : merged_policy)
				w /= static_cast<float>(policy_workers);
		prof::PrintPhase("tirages");
		double secs = MsSince(t0) / 1000.0;
		spent += secs * 1000.0;
		if(greedy.rollouts)
			std::printf("  glouton+nouveaute : %8llu tirages %10llu etats  "
						"%7llu coupures nouveaute  best %u/%zu, %u mon.\n",
						(unsigned long long)greedy.rollouts,
						(unsigned long long)greedy.nodes,
						(unsigned long long)greedy.cuts, greedy.overlap,
						target.codes.size(), greedy.monsters);
		if(nrpa.rollouts)
			std::printf("  NRPA              : %8llu tirages %10llu etats  "
						"%7llu adaptations         best %u/%zu, %u mon.\n",
						(unsigned long long)nrpa.rollouts,
						(unsigned long long)nrpa.nodes,
						(unsigned long long)nrpa.adapts, nrpa.overlap,
						target.codes.size(), nrpa.monsters);
		// Le triptyque de vie des OPTIONS (piege 52) : a `prises` nul le
		// catalogue n'est jamais choisi (poids ou applicabilite), a `avortees`
		// dominant il ne correspond pas aux prompts rencontres. Le gain
		// d'exposant est absorbees/prise (vise : longueur moyenne - 1).
		if(greedy.macro_taken + nrpa.macro_taken + greedy.macro_aborted +
		   nrpa.macro_aborted)
			std::printf("      options : %llu prises, %llu decisions absorbees "
						"(%.1f/prise), %llu avortees\n",
						(unsigned long long)(greedy.macro_taken +
											 nrpa.macro_taken),
						(unsigned long long)(greedy.macro_absorbed +
											 nrpa.macro_absorbed),
						(greedy.macro_taken + nrpa.macro_taken)
							? double(greedy.macro_absorbed +
									 nrpa.macro_absorbed) /
								  double(greedy.macro_taken + nrpa.macro_taken)
							: 0.0,
						(unsigned long long)(greedy.macro_aborted +
											 nrpa.macro_aborted));
		// La vie du NIVEAU CONTEXTUEL. Sous --mcps c'est la lecture qui dit si
		// le conditionnement par le chemin a eu la place d'apprendre : au
		// plafond il degrade vers le poids global et l'A/B ne mesure plus le
		// mecanisme du papier mais sa version tronquee.
		if(opt.ctx_shrink >= 0 && nrpa.ctx_entries)
			std::printf("      niveau contextuel : %zu case(s) au plus grand "
						"worker%s  (conditionnement : %s)\n",
						nrpa.ctx_entries,
						nrpa.ctx_capped ? "  !! PLAFOND ATTEINT (--ctx-max)" : "",
"posees+main");
		// --- LA VIE DU BANDIT DE TETE, ET SA SONDE (--qhat) ---
		//
		// Trois lectures avant toute autre. (1) `decisions` a zero = le bandit
		// n'a jamais decide (profondeur nulle, ou plafond de noeuds atteint des
		// le debut : `repli` le dit). (2) la recompense MOYENNE de la fenetre :
		// collee a zero, elle signifie que tous les tirages se valent et que Q^
		// ne peut rien separer — le mecanisme serait alors inerte quoi qu'il
		// arrive. (3) la MEMOIRE, payee par worker : c'est le seul cout du
		// mecanisme et il ne doit pas se regler a l'aveugle.
		if(opt.qhat_depth && nrpa.qhat_playouts) {
			std::printf("      bandit Q^ (--qhat %u) : %llu decisions dont "
						"%llu a la 1re, %llu tirages en fenetre, recompense "
						"moyenne %.3f\n",
						opt.qhat_depth,
						(unsigned long long)nrpa.qhat_decisions,
						(unsigned long long)nrpa.qhat_first,
						(unsigned long long)nrpa.qhat_playouts,
						nrpa.qhat_reward_sum /
							double(nrpa.qhat_playouts ? nrpa.qhat_playouts : 1));
			std::printf("                       arbre %zu noeud(s) au plus "
						"grand worker, %zu code(s) en fenetre, %.1f Mo au "
						"total%s\n",
						nrpa.qhat_nodes, nrpa.qhat_codes,
						double(nrpa.qhat_bytes) / (1024.0 * 1024.0),
						nrpa.qhat_fallback
							? "  !! PLAFOND DE NOEUDS (--qhat-nodes)" : "");
		}
		// LA SONDE. La courbe d'accord du corpus ne peut PAS juger Q^ : elle
		// mesure la reproduction d'un corpus qui ne contient QUE des bonnes
		// lignes, alors que Q^ tire son signal des ECHECS. Ceci est donc le
		// seul instrument gratuit qui reponde a « le solveur concentre-t-il sur
		// la bonne ouverture ? ». Si la bonne cible n'y ressort pas nettement,
		// aucun A/B n'est utile.
		if(opt.qhat_depth && opt.qhat_probe && !qhat_root.empty()) {
			std::vector<BanditProbe> rows;
			rows.reserve(qhat_root.size());
			for(const auto& [k, b] : qhat_root)
				rows.push_back(b);
			auto val = [](const BanditProbe& b) {
				const double d = double(b.n) + double(b.nhat);
				return d > 0 ? (b.w + b.qhat_sum) / d : 0.0;
			};
			auto ligne = [&](const BanditProbe& b) {
				std::string name = b.code ? db.Name(b.code) : std::string("-");
				if(name.empty())
					name = std::to_string(b.code);
				std::printf("      %-38.38s %9u %6.3f %9u %6.3f %6.3f\n",
							name.c_str(), b.n,
							b.n ? b.w / double(b.n) : 0.0, b.nhat,
							b.nhat ? b.qhat_sum / double(b.nhat) : 0.0,
							val(b));
			};
			auto entete = [&](const char* titre) {
				std::printf("\n  --- %s ---\n", titre);
				std::printf("      %-38.38s %9s %6s %9s %6s %6s\n",
							"coup", "n", "Q", "n^", "Q^", "val");
			};
			// TABLE 1 — CE QUE LE BANDIT DECIDE a la premiere decision. Elle
			// est courte par nature (les ouvertures d'un prompt idle) et sert
			// surtout a verifier que le mecanisme decide bien quelque chose.
			std::vector<BanditProbe> dec;
			for(const BanditProbe& b : rows)
				if(b.n)
					dec.push_back(b);
			std::sort(dec.begin(), dec.end(),
					  [&](const BanditProbe& a, const BanditProbe& b) {
						  return val(a) > val(b);
					  });
			entete("sonde du bandit : DECISIONS A LA RACINE (s = {})");
			for(size_t i = 0; i < dec.size() && i < 8; ++i)
				ligne(dec[i]);
			// TABLE 2 — LA STATISTIQUE DE PERMUTATION ELLE-MEME, Q^({}, a) :
			// la recompense moyenne des lignes ayant joue a, n'importe ou et
			// dans n'importe quel ordre, MOYENNEE SUR TOUS LES TIRAGES, y
			// compris les mauvais. C'est LA lecture du chantier : si la bonne
			// cible d'un tuteur (Tenki) n'y ressort pas nettement au bout de
			// quelques milliers de tirages, le mecanisme ne separe rien et
			// aucun A/B n'est utile. Les coups a effectif famelique sont
			// ecartes : une moyenne sur trois tirages n'est pas une moyenne.
			uint32_t seuil = 0;
			for(const BanditProbe& b : rows)
				seuil = (std::max)(seuil, b.nhat);
			seuil = seuil / 100 + 1;   // 1 % du coup le plus frequent
			std::vector<BanditProbe> perm;
			for(const BanditProbe& b : rows)
				if(b.nhat >= seuil)
					perm.push_back(b);
			std::sort(perm.begin(), perm.end(),
					  [](const BanditProbe& a, const BanditProbe& b) {
						  const double qa =
							  a.nhat ? a.qhat_sum / double(a.nhat) : 0.0;
						  const double qb =
							  b.nhat ? b.qhat_sum / double(b.nhat) : 0.0;
						  return qa > qb;
					  });
			std::printf("\n  --- sonde du bandit : PERMUTATION Q^({}, a) sur "
						"%zu coup(s) d'effectif >= %u ---\n",
						perm.size(), seuil);
			std::printf("      %-38.38s %9s %6s %9s %6s %6s\n",
						"coup", "n", "Q", "n^", "Q^", "val");
			const size_t show = perm.size() < 24 ? perm.size() : size_t(24);
			for(size_t i = 0; i < show; ++i)
				ligne(perm[i]);
			if(perm.size() > show) {
				std::printf("      ... %zu de plus, dont les PIRES :\n",
							perm.size() - show);
				for(size_t i = perm.size() < 4 ? 0 : perm.size() - 4;
					i < perm.size(); ++i)
					ligne(perm[i]);
			}
		}
		// La vie du MINAGE EN LIGNE. Trois lectures qui decident de son sort :
		// le nombre de tours (a zero, le run n'a jamais eu de quoi miner), le
		// catalogue final (taille et perte modele — la meme lecture que le
		// catalogue statique), et la DUREE du minage, qui court dans le budget
		// du run : au-dessus de la seconde, il faudrait rendre le corpus vivant
		// plus petit ou la periode plus longue.
		if(opt.options_online) {
			std::lock_guard<std::mutex> lock(online.mu);
			std::printf("      options en ligne : %u tour(s) de minage, corpus "
						"vivant %zu ligne(s) (%llu offertes, %llu retenues, "
						"%llu doublons)\n",
						online.rounds, online.pool.size(),
						(unsigned long long)online.offered,
						(unsigned long long)online.kept,
						(unsigned long long)online.dups);
			if(online.rounds)
				std::printf("                       dernier catalogue : %zu macro(s) "
							"(moyenne %.1f, max %zu) sur %zu ligne(s), perte modele "
							"%.1f -> %.1f log10 ; minage %.0f ms en moyenne, %.0f ms "
							"au pire\n",
							online.last_size, online.last_avglen,
							online.last_maxlen, online.last_lines,
							online.last_flat, online.last_opt,
							online.mine_ms_total / double(online.rounds),
							online.mine_ms_max);
			// Le catalogue mine en ligne SURVIT aux tirages : c'est la meilleure
			// connaissance de macros que le run possede, et les tirages enracines
			// du finisseur doivent en heriter — sinon le run se desarmerait
			// exactement au moment ou il convertit. Plus personne ne re-mine
			// apres ce point : le catalogue redevient statique.
			if(online.cat && online.cat->Size()) {
				final_online = online.cat;
				cfg.options = final_online.get();
			}
		}
		// Ce que les CONTRAINTES DE LIGNE coupent, PAR MODE. Trois mecanismes
		// actifs dans tous les runs disciplines depuis la session 3, et aucun
		// n'etait imprime LA OU IL TRAVAILLE : `PrintCuts` ne couvrait que les
		// passes LDS, ou ils valent zero par construction (audit 1.1-1.3).
		// `garde` repond a la question laissee ouverte par le §9.11 ;
		// `contrainte` dit combien de tirages --resolve/--summon-min tuent ;
		// `tour` separe « budget de decisions trop court » de « la ligne
		// deborde du tour 1 ».
		for(int mi = 0; mi < 2; ++mi) {
			const ModeStats& m = mi ? nrpa : greedy;
			if(!m.rollouts)
				continue;
			std::printf("      %-7s coupures : contrainte %llu, garde %llu, "
						"tour %llu   (%.0f%% des tirages)\n",
						mi ? "NRPA" : "glouton",
						(unsigned long long)m.constraint_cuts,
						(unsigned long long)m.guard_cuts,
						(unsigned long long)m.turn_cuts,
						100.0 * double(m.constraint_cuts + m.guard_cuts +
									   m.turn_cuts) / double(m.rollouts));
			// Vie de --no-self-negate (s22ter).
			if(m.self_negate_cuts)
				std::printf("      %-7s discipline : %llu negation(s) sur soi "
							"retiree(s)\n", mi ? "NRPA" : "glouton",
							(unsigned long long)m.self_negate_cuts);
		}
		// PROFIL DE PROGRESSION PAR TIRAGE (s21) — l'instrument que 9.32
		// nommait, et le verdict qu'il rend est binaire : un PIC unique dans
		// l'histogramme = tous les tirages meurent au meme barreau de
		// l'echelle x* (un verrou NOMMABLE, chercher l'option manquante a ce
		// palier) ; une DISPERSION = c'est l'arite qui tue, et la forme close
		// (cout = Sigma b^(l_i), domine par b^(l_max)) dit qu'il faut couper
		// plus fin, pas chercher un mecanisme de plus.
		for(int mi = 0; mi < 2; ++mi) {
			const ModeStats& m = mi ? nrpa : greedy;
			if(!m.sp_lines)
				continue;
			int hi = 71;
			while(hi > 0 && !m.sp_final[hi])
				--hi;
			int mode_k = 0;
			uint64_t mode_n = 0;
			std::printf("      %-7s profil de progression (max de SerialProgress "
						"par tirage, %llu mesures) :\n",
						mi ? "NRPA" : "glouton",
						(unsigned long long)m.sp_lines);
			for(int k = 0; k <= hi; ++k) {
				if(!m.sp_final[k])
					continue;
				if(m.sp_final[k] > mode_n) {
					mode_n = m.sp_final[k];
					mode_k = k;
				}
				std::printf("        %2d unite(s) : %8llu  (%5.1f %%)\n", k,
							(unsigned long long)m.sp_final[k],
							100.0 * double(m.sp_final[k]) / double(m.sp_lines));
			}
			std::printf("        derniere decision de progres : %.1f en "
						"moyenne ; verdict : %s\n",
						double(m.sp_at_sum) / double(m.sp_lines),
						2 * mode_n > m.sp_lines
							? "PIC UNIQUE — verrou nommable a ce barreau"
							: "DISPERSION — l'arite tue, couper plus fin");
		}
		// LA VIE DU RETOUR AU BARREAU (--reenter, piege 52). A zero re-entree
		// avec le drapeau arme, le mecanisme est inerte (archive vide ou
		// serialisation absente) et aucun juge de recherche ne le concerne.
		// Un rejeu ECHOUE n'est pas du bruit : un chemin d'archive doit se
		// rejouer depuis la racine, l'echec est un defaut a regarder.
		for(int mi = 0; mi < 2; ++mi) {
			const ModeStats& m = mi ? nrpa : greedy;
			if(!m.reenter_rollouts && !m.reenter_fail)
				continue;
			std::printf("      %-7s retour au barreau : %llu tirage(s) "
						"re-entre(s) (%.1f %% des tirages), base moyenne "
						"%.1f unite(s)%s",
						mi ? "NRPA" : "glouton",
						(unsigned long long)m.reenter_rollouts,
						m.rollouts ? 100.0 * double(m.reenter_rollouts) /
										 double(m.rollouts)
								   : 0.0,
						m.reenter_rollouts
							? double(m.reenter_base_sum) /
								  double(m.reenter_rollouts)
							: 0.0,
						m.reenter_fail ? "" : "\n");
			if(m.reenter_fail)
				std::printf(", %llu rejeu(x) ECHOUE(S) <-- defaut\n",
							(unsigned long long)m.reenter_fail);
		}
		// Vie du raffinement (s22, chantier 3). `top_hits` a zero avec des
		// workers raffines = la sous-echelle est posee mais jamais foulee.
		if(greedy.refine_done + nrpa.refine_done)
			std::printf("      raffinement : %llu worker(s) re-serialise(s), "
						"%llu sous-barreau(x) au total, %llu etat(s) "
						"archive(s) au-dela de la porte\n",
						(unsigned long long)(greedy.refine_done +
											 nrpa.refine_done),
						(unsigned long long)(greedy.refine_subrungs +
											 nrpa.refine_subrungs),
						(unsigned long long)(greedy.refine_top_hits +
											 nrpa.refine_top_hits));
		std::printf("  total : %.1f s, au mieux %u des %zu cartes cibles, "
					"%u monstre(s)\n", secs, best_overlap, target.codes.size(),
					best_monsters);
		if(!cfg.hint_cards.empty()) {
			const uint64_t hs = nrpa.hint_seen + greedy.hint_seen;
			const uint64_t hx = nrpa.hint_exact + greedy.hint_exact;
			const uint64_t hl = nrpa.hint_sel + greedy.hint_sel;
			std::printf("  visibilite des indices : legaux dans %llu etat(s), "
						"pris %llu fois%s\n",
						(unsigned long long)hs,
						(unsigned long long)(nrpa.hint_taken + greedy.hint_taken),
						hs == 0 ? "  <-- JAMAIS LEGAL : le probleme est la "
								  "disponibilite des materiaux, pas la recherche"
								: "");
			// VENTILATION (audit 18) : sans elle ce compteur avait TROIS causes
			// — le drapeau --card-on-select, la qualite du run et le volume de
			// travail — et ne pouvait donc en juger aucune. Seule la colonne
			// `coup exact` parle d'un coup ; la colonne `sous-ens.` n'existe que
			// sous --card-on-select et y designe le premier code d'une
			// selection, pas la carte engagee.
			std::printf("     dont coup EXACT (idle/chaine/position) %llu, "
						"sous-ensemble (identite approximative) %llu\n",
						(unsigned long long)hx, (unsigned long long)hl);
		}
		// VIE DE --adapt-to-peak (piege 52) : a zero le mecanisme est INERTE et
		// aucun juge de recherche ne le concerne.
		if(cfg.adapt_to_peak)
			std::printf("  gradient tronque au pic : %llu pas retires\n",
						(unsigned long long)(nrpa.peak_trunc +
											 greedy.peak_trunc));
		// TRAVAIL REFAIT PAR LA TRANSPOSITION (audit 18) : etats deja vus mais
		// avec un budget plus petit, donc RE-DEVELOPPES. Le dossier ne comptait
		// que la coupure et ne pouvait pas dire si le mecanisme paie.
		if(nrpa.tt_reexplored + greedy.tt_reexplored)
			std::printf("  transposition : %llu etat(s) RE-EXPLORES faute de "
						"budget a la premiere visite\n",
						(unsigned long long)(nrpa.tt_reexplored +
											 greedy.tt_reexplored));
		// LE diagnostic du handrip : des tirages atteignent-ils seulement UNE
		// resolution exigee ? Zero a >=1 = le rip n'est jamais legal/possible
		// (jeu) ; des >=1 sans >=3 = la sequence complete est hors de portee
		// de l'echantillonnage (recherche).
		if(!cons.resolve_min.empty()) {
			std::printf("  resolutions atteintes par tirage : >=1 %llu  >=2 %llu"
						"  >=3 %llu  >=4 %llu%s\n",
						(unsigned long long)(nrpa.rr[0] + greedy.rr[0]),
						(unsigned long long)(nrpa.rr[1] + greedy.rr[1]),
						(unsigned long long)(nrpa.rr[2] + greedy.rr[2]),
						(unsigned long long)(nrpa.rr[3] + greedy.rr[3]),
						(nrpa.rr[0] + greedy.rr[0]) == 0
							? "  <-- JAMAIS : rip illegal ou hors de portee "
							  "depuis ce depart"
							: "");
			// La mesure qui departage recherche et ressources : jusqu'ou les
			// lignes AUX RESOLUTIONS COMPLETES montent-elles ?
			std::printf("  meilleure crete AUX resolutions completes : %u/%zu\n",
						(std::max)(nrpa.overlap_ripped, greedy.overlap_ripped),
						target.codes.size());
		}
		// LANDMARKS : le `h` appris DESCEND-IL ? C'est le critere INTERNE du
		// mecanisme, et il se lit ici — dans la phase tirages, ou le poids
		// travaille. Colle au total appris : la recherche n'accomplit rien.
		// Colle a zero : les landmarks sont trop faciles et ne guident pas.
		// Sans cette ligne, un `h` allume est indiscernable d'un `h` inerte.
		if(nrpa.lm_h_count + greedy.lm_h_count) {
			const double s0 = nrpa.lm_h_sum + greedy.lm_h_sum;
			const uint64_t n0 = nrpa.lm_h_count + greedy.lm_h_count;
			std::printf("  landmarks : h moyen %.2f sur %zu appris, %llu "
						"evaluation(s) dans les tirages\n",
						s0 / double(n0),
						landmark_graph.Items().size(),
						(unsigned long long)n0);
		}
		// SESSION 17 : LA VIE DES QUATRE MECANISMES, dans la phase ou ils
		// travaillent. La question n'est pas « le drapeau etait-il allume » mais
		// « la distance a-t-elle REELLEMENT decru » et « la decomposition
		// avance-t-elle » — un mecanisme vivant et inerte est la faute que la
		// session 16 a commise deux fois.
		if(nrpa.recipe_snaps + greedy.recipe_snaps) {
			std::printf("  recettes : instantane %llu fois — %llu produit(s), "
						"%llu code(s) utile(s), %llu sous-produit(s) a rebours\n",
						(unsigned long long)(nrpa.recipe_snaps +
											 greedy.recipe_snaps),
						(unsigned long long)(std::max)(nrpa.snap_products,
													   greedy.snap_products),
						(unsigned long long)(std::max)(nrpa.snap_useful,
													   greedy.snap_useful),
						(unsigned long long)(std::max)(nrpa.snap_backward,
													   greedy.snap_backward));
		}
		// LA VIE DU BIAIS D'OPERATEUR (--op-bias). Imprimee AVANT tout juge de
		// recherche : a `proposé = 0`, le mecanisme est INERTE et un A/B
		// mesurerait deux fois le temoin (piege 42, 9.26 (e)).
		if(opt.op_bias > 0.0) {
			const uint64_t off = nrpa.op_offered + greedy.op_offered;
			const uint64_t tak = nrpa.op_taken + greedy.op_taken;
			std::printf("  biais d'OPERATEUR : %llu carte(s) designee(s) par la "
						"decomposition ; %llu decision(s) en offraient une, "
						"%llu l'ont prise (%.2f %%)%s\n",
						(unsigned long long)(std::max)(nrpa.op_listed,
													   greedy.op_listed),
						(unsigned long long)off, (unsigned long long)tak,
						off ? 100.0 * double(tak) / double(off) : 0.0,
						off ? "" : "   <-- INERTE : aucun juge ne le concerne");
		}
		if(nrpa.rec_roll_count + greedy.rec_roll_count) {
			const uint64_t n1 = nrpa.rec_roll_count + greedy.rec_roll_count;
			const double d = (nrpa.rec_roll_sum + greedy.rec_roll_sum) /
							 double(n1);
			const double d0 = (nrpa.rec_roll_d0_sum + greedy.rec_roll_d0_sum) /
							  double(n1);
			std::printf("  recettes : distance moyenne %.2f contre %.2f au "
						"depart (%llu evaluation(s) dans les tirages)%s\n",
						d, d0, (unsigned long long)n1,
						d >= d0 ? "  <-- N'A PAS DECRU" : "");
		}
		if(nrpa.backward_count + greedy.backward_count) {
			const uint64_t n2 = nrpa.backward_count + greedy.backward_count;
			std::printf("  rebours : %.2f sous-produit(s) fabrique(s) en moyenne "
						"sur %llu (%llu evaluation(s))\n",
						(nrpa.backward_sum + greedy.backward_sum) / double(n2),
						(unsigned long long)(std::max)(nrpa.snap_backward,
													   greedy.snap_backward),
						(unsigned long long)n2);
		}
		if(nrpa.hindsight_goals + greedy.hindsight_goals ||
		   opt.hindsight > 0.0) {
			std::printf("  hindsight : %llu but(s) de substitution retenu(s), "
						"%llu adaptation(s)%s\n",
						(unsigned long long)(nrpa.hindsight_goals +
											 greedy.hindsight_goals),
						(unsigned long long)(nrpa.hindsight_adapts +
											 greedy.hindsight_adapts),
						(nrpa.hindsight_goals + greedy.hindsight_goals) == 0
							? "  <-- AUCUN : aucun monstre d'extra deck invoque"
							: "");
			// Ventilation s22 (chantier 5.4) : hindsight renforce-t-il les
			// fusions qui ont DEPENSE un quota suivi ? Mesure, pas correctif.
			const uint64_t hq_sp = nrpa.hindsight_quota_spent +
								   greedy.hindsight_quota_spent;
			const uint64_t hq_fr = nrpa.hindsight_quota_fresh +
								   greedy.hindsight_quota_fresh;
			if(hq_sp + hq_fr)
				std::printf("      ventiles par quota suivi : %llu commis "
							"quota DEPENSE (%.0f %%), %llu quotas frais\n",
							(unsigned long long)hq_sp,
							100.0 * double(hq_sp) / double(hq_sp + hq_fr),
							(unsigned long long)hq_fr);
		}
		// SONDE DE REPETITION (session 16) : l'instrument qui separe « le 2e
		// exemplaire n'est JAMAIS TENTE » (le materiau etait la — panne
		// d'echantillonnage) de « il est TOUJOURS PERDU » (la chaine etait
		// consommee — panne de h). Les deux appellent des chantiers opposes et
		// `best_overlap` ne les separe pas.
		if(opt.probe_repeat) {
			RepeatProbe both[4];
			for(int i = 0; i < 4; ++i) {
				// Les deux modes s'additionnent : meme question, meme unite.
				both[i] = nrpa.rep[i];
				const RepeatProbe& g = greedy.rep[i];
				if(!both[i].code)
					both[i].code = g.code;
				for(int k = 0; k < 5; ++k)
					both[i].reached[k] += g.reached[k];
				both[i].more_n += g.more_n;
				both[i].more_sum += g.more_sum;
				both[i].rest_sum += g.rest_sum;
				both[i].more_kept += g.more_kept;
				both[i].more_lost += g.more_lost;
				both[i].after_sum += g.after_sum;
				both[i].first_depth_sum += g.first_depth_sum;
				both[i].d0_samples += g.d0_samples;
				both[i].known |= g.known;
				if(g.more_n) {
					both[i].more_min = (std::min)(both[i].more_min, g.more_min);
					both[i].more_max = (std::max)(both[i].more_max, g.more_max);
				}
				if(g.d0 != 0xffffffffu &&
				   (both[i].d0 == 0xffffffffu || g.d0 > both[i].d0)) {
					both[i].d0 = g.d0;
					both[i].rest0 = g.rest0;
				}
			}
			PrintRepeatProbe(both, nrpa.rollouts + greedy.rollouts, db,
							 true, true,
							 "phase TIRAGES");
		}
		// Session 6 : la borne B&B ne tourne plus en aveugle — atteintes du
		// but (re-atteintes d'apres-but comprises, piege 35) et coupures.
		if(opt.optimize) {
			uint32_t bb = opt.burn_limit ? opt.burn_limit : UINT32_MAX;
			for(const Solution& s : sols)
				bb = (std::min)(bb, s.burned);
			char bstr[32] = "aucune";
			if(bb != UINT32_MAX)
				std::snprintf(bstr, sizeof(bstr), "%u (marge +%u)", bb,
							  opt.burn_slack);
			std::printf("  anytime : %llu atteinte(s) du but, %llu coupure(s) "
						"borne brulees, meilleures brulees %s%s\n",
						(unsigned long long)(nrpa.goal_hits + greedy.goal_hits),
						(unsigned long long)(nrpa.burn_cuts + greedy.burn_cuts),
						bstr, opt.burn_share ? "" : "  [partage OFF]");
		}
		if(!sols.empty())
			std::printf("  %zu ligne(s) atteignant le board.\n", sols.size());
	}

	// --- 4c. FINISSEUR. Les tirages savent MONTER — mesures : trois runs sur
	// trois s'arretent a 7-8 cartes sur 8 — mais le dernier pas (convertir un
	// corps en la carte manquante, corriger une position) est une aiguille que
	// l'echantillonnage ne trouve pas. Deux moteurs (--finisher) :
	//   - mono (l'ancien) : fouille GUIDEE depuis le SEUL meilleur etat —
	//     mesure trois fois epuise en ~6 etats, l'espace y est verrouille des
	//     l'invocation, fouiller l'etat final ne peut pas le corriger ;
	//   - levin (defaut) : archive Go-Explore (arXiv:2004.12919) + prefixes de
	//     recul + Levin Tree Search (arXiv:2103.11505) sur la politique NRPA —
	//     K racines DISTINCTES, dont des etats d'AVANT le verrouillage, et
	//     depuis chacune une recherche best-first complete ordonnee par la
	//     politique apprise. --finisher ab : les deux a budget egal, la mesure.

	// Rejoue un prefixe en comptant ce que la recherche devra savoir :
	// invocations (contraintes), tours (coupure), resolutions (--resolve),
	// actions (cout des solutions completes).
	struct PrefixCount {
		uint32_t actions = 0, summons = 0, turns = 0;
		uint64_t resolved = 0;
		size_t used = 0;
		bool ok = false;
	};
	auto replay_prefix = [&](Duel& fd,
							 const std::vector<std::vector<uint8_t>>& pre)
		-> PrefixCount {
		// Le cout du rejeu de prefixe (finisseur : une fois par racine) a sa
		// propre sonde ; son self exclut les Process internes.
		prof::Scope ps(prof::kPrefix);
		PrefixCount pc;
		bool retry = false;
		while(pc.used < pre.size() && !retry) {
			int st = fd.Process();
			for(const Message& m : fd.Messages()) {
				switch(m.type) {
				case MSG_SUMMONING:
				case MSG_SPSUMMONING:
					++pc.summons;
					++pc.actions;
					// Invocations surveillees (--summon-min) du prefixe.
					if(!cons.resolve_min.empty() && m.size >= 4) {
						uint32_t c = 0;
						std::memcpy(&c, m.data, 4);
						if(c) {
							const uint32_t sc = db.Canonical(c);
							for(size_t k = 0; k < cons.resolve_min.size(); ++k)
								if(cons.resolve_min[k].on_summon &&
								   cons.resolve_min[k].code == sc)
									pc.resolved += 1ull << (16 * k);
						}
					}
					break;
				case MSG_FLIPSUMMONING:
					++pc.actions;
					break;
				case MSG_CHAINING:
					if(!cons.resolve_min.empty() && m.size >= 4) {
						uint32_t c = 0;
						std::memcpy(&c, m.data, 4);
						c = db.Canonical(c);
						const uint32_t loc = ChainingLocation(m.data, m.size);
						for(size_t k = 0; k < cons.resolve_min.size(); ++k)
							if(!cons.resolve_min[k].on_summon &&
							   cons.resolve_min[k].code == c &&
							   (!cons.resolve_min[k].zones ||
								(loc & cons.resolve_min[k].zones)))
								pc.resolved += 1ull << (16 * k);
					}
					++pc.actions;
					break;
				case MSG_NEW_TURN:
					++pc.turns;
					break;
				case MSG_RETRY:
					retry = true;
					break;
				default:
					break;
				}
			}
			if(st == OCG_DUEL_STATUS_AWAITING)
				fd.SetResponse(pre[pc.used++]);
			else if(st != OCG_DUEL_STATUS_CONTINUE)
				break;
		}
		pc.ok = !retry && pc.used == pre.size();
		return pc;
	};

	// Solutions issues des racines d'APPROCHE (--approach). Elles se rejouent
	// sur le duel de l'approche (l'en-tete de son fichier), pas forcement sur
	// le duel de depart : sur un depart hand test (--start), l'en-tete yrp1
	// pseudo-melange diverge du reload — mesure : l'approche 8/8 de la
	// session 3 se juge 239/239 mais meurt a 39/239 rejouee sur le duel de
	// depart. Elles s'ecrivent donc contre LEUR en-tete, et se jugent avec le
	// meme --opp-hand, comme n'importe quel replay produit.
	struct ApproachSols {
		std::unique_ptr<Replay> holder;
		std::string file;
		std::vector<Solution> sols;
	};
	std::vector<ApproachSols> approach_runs;

	// L'ancien finisseur, conserve tel quel pour l'A/B (--finisher mono|ab).
	auto run_mono = [&](double budget) {
		std::printf("\n--- finisseur mono : fouille guidee depuis le meilleur "
					"etat (%u/%zu, %zu decisions) ---\n", best_overlap,
					target.codes.size(), best_path.size());
		std::thread([&] {
			Arena fa;
			std::string err;
			if(!fa.Init(opt.arena_mb << 20, 0, err))
				return WorkerAbort("arene (finisseur mono guide)", err);
			{
				Duel fd(db, scripts, &fa);
				if(!fd.Create(start_yrp.seed, start_yrp.duel_flags,
							  start_yrp.start_lp, start_yrp.start_hand,
							  start_yrp.draw_count, err) ||
				   !fd.Setup(start_yrp, err,
							 cons.opp_hand.empty() ? nullptr : &cons.opp_hand,
							 static_cast<uint8_t>(1 - opt.target_player)))
					return WorkerAbort("duel (finisseur mono guide)", err);
				if(opt.stop_gc)
					fd.SetLuaGc(false);
				PrefixCount pc = replay_prefix(fd, best_path);
				if(!pc.ok) {
					std::printf("  !! le prefixe ne se rejoue pas (%zu/%zu) : "
								"finisseur annule\n", pc.used, best_path.size());
				} else {
					SearchConfig fcfg = cfg;
					fcfg.time_limit_ms = budget;
					fcfg.max_decisions =
						FinisherDepth(cfg.max_decisions, best_path.size());
					fcfg.initial_summons = pc.summons;
					fcfg.initial_turns = pc.turns;
					fcfg.initial_resolved = pc.resolved;
					fcfg.max_solutions = 8;
					Search fs(fd, fa, start_yrp, fcfg);
					fs.RunGuided(target);
					std::printf("  %llu etats en %.1f s, au mieux %u/%zu  (%s)\n",
								(unsigned long long)fs.Stats().nodes,
								fs.Stats().ms / 1000.0,
								fs.Stats().best_overlap, target.codes.size(),
								SearchOutcome(fs.Stats()));
					PrintCuts([&] {
						CutCounts c;
						c.Add(fs.Stats());
						return c;
					}());
					for(Solution x : fs.Solutions()) {
						// La solution complete = prefixe + suffixe trouve.
						std::vector<std::vector<uint8_t>> full = best_path;
						full.insert(full.end(), x.responses.begin(),
									x.responses.end());
						x.responses = std::move(full);
						x.decisions += static_cast<uint32_t>(best_path.size());
						x.actions += pc.actions;
						sols.push_back(std::move(x));
					}
				}
			}
			ReportPoison("finisseur mono guide", fa);
			fa.Shutdown();
		}).join();
		prof::PrintPhase("finisseur mono");
	};

	// Le finisseur archive + LTS : racines = archive triee par score, puis le
	// meilleur chemin global et ses prefixes de recul (N decisions retirees —
	// des etats d'AVANT le verrouillage, l'approximation praticable du
	// re-rooting de sqrtLTS, arXiv:2412.05196).
	auto run_levin = [&](double budget) {
		struct FinishRoot {
			std::string label;
			uint32_t overlap;   // 0 = inconnu (recul)
			std::vector<std::vector<uint8_t>> pre;
		};
		std::vector<FinishRoot> roots;
		auto add_root = [&](std::string label, uint32_t overlap,
							std::vector<std::vector<uint8_t>> p) {
			if(p.empty())
				return;
			for(const FinishRoot& r : roots)
				if(r.pre == p)
					return;
			roots.push_back({ std::move(label), overlap, std::move(p) });
		};
		{
			std::vector<const ArchiveEntry*> ents;
			ents.reserve(global_archive.size());
			for(const auto& [cell, e] : global_archive)
				ents.push_back(&e);
			std::sort(ents.begin(), ents.end(),
					  [](const ArchiveEntry* a, const ArchiveEntry* b) {
						  return a->score > b->score;
					  });
			if(opt.archive_k && ents.size() > opt.archive_k)
				ents.resize(opt.archive_k);
			char lbl[48];
			for(size_t i = 0; i < ents.size(); ++i) {
				std::snprintf(lbl, sizeof(lbl), "archive %02zu %u/%zu r%u", i,
							  ents[i]->overlap, target.codes.size(),
							  ents[i]->resolves);
				add_root(lbl, ents[i]->overlap, ents[i]->path);
			}
			// Les meilleurs etats d'archive sont massivement VERROUILLES
			// (mesure : 1-3 expansions puis epuisement — le verrou se pose a
			// l'invocation, bien avant l'etat final) ; leurs prefixes de
			// recul, eux, demarrent avant le verrou. TOP-12 (s21) : trois
			// cellules ne couvraient qu'un sommet de l'echelle fine — la
			// conjonction du but (habilitants + materiaux + quota) peut vivre
			// dans n'importe laquelle des cellules-frontiere.
			// Reculs 45/60 (s21) : le vecteur des cellules-frontiere porte la
			// CONJONCTION (Leo@cimetiere + habilitants en jeu) mais l'etat est
			// verrouille — et le quota 1/tour de Wolf, INVISIBLE au vecteur,
			// se depense ~30-50 reponses avant la fin. Il faut reculer
			// jusqu'AVANT la depense.
			// BALISE (s22, chantier 5) : les reculs {15,30,45,60} et le TOP-12
			// sont CALES SUR L'ETALON A et jamais balayes — comme les tranches
			// de departs (2 x 15), le tournoi de 2 et le plafond de 12 hotes a
			// quota. Aucun n'est faux ; aucun n'est derive. Ne les balayer QUE
			// si un juge gratuit le demande.
			for(size_t i = 0; i < ents.size() && i < 12; ++i)
				for(uint32_t back : { 15u, 30u, 45u, 60u })
					if(ents[i]->path.size() > back) {
						std::snprintf(lbl, sizeof(lbl), "arch%02zu recul %u", i,
									  back);
						add_root(lbl, 0,
								 std::vector<std::vector<uint8_t>>(
									 ents[i]->path.begin(),
									 ents[i]->path.end() - back));
					}
		}
		add_root("meilleur", best_overlap, best_path);
		for(uint32_t back : { 5u, 10u, 20u, 40u, 60u, 90u })
			if(best_path.size() > back)
				add_root("recul " + std::to_string(back), 0,
						 std::vector<std::vector<uint8_t>>(
							 best_path.begin(), best_path.end() - back));
		// Optimisation : les prefixes des solutions les MOINS CHERES sont les
		// meilleures racines — perturber la fin d'une ligne complete qui coute
		// deja peu, la ou un recul qui economise UNE brulee est une victoire.
		uint32_t best_known_burn = opt.burn_limit;
		if(opt.optimize && !sols.empty()) {
			std::vector<const Solution*> cheap;
			cheap.reserve(sols.size());
			for(const Solution& s : sols)
				cheap.push_back(&s);
			std::sort(cheap.begin(), cheap.end(),
					  [](const Solution* a, const Solution* b) {
						  return std::tie(a->burned, a->actions, a->decisions) <
								 std::tie(b->burned, b->actions, b->decisions);
					  });
			if(!best_known_burn || cheap[0]->burned < best_known_burn)
				best_known_burn = cheap[0]->burned;
			// La borne partagee herite du meilleur cout d'avant-finisseur.
			if(opt.burn_share && best_known_burn) {
				uint32_t cur = shared_burn.load();
				while(best_known_burn < cur &&
					  !shared_burn.compare_exchange_weak(cur, best_known_burn)) {}
			}
			char slbl[48];
			for(size_t i = 0; i < cheap.size() && i < 3; ++i)
				for(uint32_t back : { 30u, 60u, 90u, 120u })
					if(cheap[i]->responses.size() > back) {
						std::snprintf(slbl, sizeof(slbl), "sol%zu(b%u) recul %u",
									  i, cheap[i]->burned, back);
						add_root(slbl, 0,
								 std::vector<std::vector<uint8_t>>(
									 cheap[i]->responses.begin(),
									 cheap[i]->responses.end() - back));
					}
		}
		if(roots.empty() && opt.approach_files.empty())
			return;
		std::printf("\n--- finisseur : LTS sur %zu racine(s) + %zu approche(s) "
					"(archive %zu cellules, politique %zu poids) ---\n",
					roots.size(), opt.approach_files.size(),
					global_archive.size(), merged_policy.size());
		auto t0 = Clock::now();
		std::atomic<size_t> next_root{ 0 };
		std::atomic<uint32_t> found{ 0 };
		// En optimisation, « quelques solutions suffisent » n'existe plus :
		// chaque racine restante peut porter une ligne MOINS CHERE.
		const uint32_t found_stop = opt.optimize ? 0x7fffffffu : 4u;
		std::mutex fmx;
		// Session 6 : compteurs de la borne B&B, agreges sur toutes les
		// racines du finisseur (LTS + tirages enracines).
		std::atomic<uint64_t> fin_goal_hits{ 0 }, fin_burn_cuts{ 0 };
		// SONDE DE REPETITION, cote FINISSEUR. La sonde de la phase tirages ne
		// voit pas les tirages ENRACINES, et c'est exactement le piege
		// d'instrument de 9.21 (d) : sur l'etalon A, la 2e Liger de la session
		// 14 avait ete trouvee la, invisible dans la ligne de resume. Une sonde
		// qui ne couvrirait que la phase tirages conclurait « jamais » sur un
		// run qui y arrive. Protegee par `fmx`, comme les impressions.
		RepeatProbe fin_rep[4];
		uint64_t fin_rollouts = 0;

		// --- phase 1 : racines d'approche, chacune sur SON duel (cf.
		// ApproachSols). Deux moteurs, choisis par la profondeur du recul :
		// les reculs COURTS passent au LTS — l'epuisement y est une PREUVE
		// d'absence en quelques secondes ; les reculs PROFONDS passent aux
		// tirages NRPA enracines (phase A2 plus bas) — la recherche
		// systematique meurt au budget a 60-150 decisions du but,
		// l'echantillonnage profond est fait pour ca.
		for(size_t a = 0; a < opt.approach_files.size(); ++a) {
			auto holder = std::make_unique<Replay>();
			std::string aerr;
			if(!holder->Load(opt.approach_files[a], aerr)) {
				std::printf("  !! --approach %s : %s\n",
							opt.approach_files[a].c_str(), aerr.c_str());
				continue;
			}
			approach_runs.push_back(
				{ std::move(holder), opt.approach_files[a], {} });
		}
		const double a1_deadline = budget * 0.2;
		const double a2_deadline = budget * 0.75;
		std::printf("  partage des phases d'approche : A1 %.0f s (0,20), "
					"A2 %.0f s (0,75), reste %.0f s\n", a1_deadline / 1000.0,
					a2_deadline / 1000.0, (budget - a2_deadline) / 1000.0);
		for(size_t a = 0; a < approach_runs.size(); ++a) {
			ApproachSols& AR = approach_runs[a];
			const Replay* ap = AR.holder->IsStreamed() ? AR.holder->Embedded()
													   : AR.holder.get();
			if(!ap || ap->responses.empty()) {
				std::printf("  !! --approach %s : pas de reponses lisibles\n",
							AR.file.c_str());
				continue;
			}
			// Reculs courts seulement : le LTS y epuise l'espace (preuve) ;
			// les reculs profonds passent a l'echantillonnage (phase A2).
			const std::vector<uint32_t> backs{ 0u, 10u, 20u, 30u, 45u };
			std::atomic<size_t> anext{ 0 };
			unsigned anw = (std::min<unsigned>)(
				threads, static_cast<unsigned>(backs.size()));
			std::vector<std::thread> apool;
			for(unsigned w = 0; w < anw; ++w) {
				apool.emplace_back([&] {
					Arena fa;
					std::string err;
					if(!fa.Init(opt.arena_mb << 20, 0, err))
						return WorkerAbort("arene (finisseur --approach)", err);
					{
						Duel fd(db, scripts, &fa);
						if(fd.Create(ap->seed, ap->duel_flags, ap->start_lp,
									 ap->start_hand, ap->draw_count, err) &&
						   fd.Setup(*ap, err,
									cons.opp_hand.empty() ? nullptr
														  : &cons.opp_hand,
									static_cast<uint8_t>(
										1 - opt.target_player))) {
							if(opt.stop_gc)
								fd.SetLuaGc(false);
							fa.Push();
							for(;;) {
								size_t i = anext.fetch_add(1);
								if(i >= backs.size())
									break;
								double left = a1_deadline - MsSince(t0);
								if(left < 2000 || found.load() >= found_stop)
									break;
								const uint32_t back = backs[i];
								if(ap->responses.size() <= back)
									continue;
								fa.Restore();
								std::vector<std::vector<uint8_t>> pre(
									ap->responses.begin(),
									ap->responses.end() - back);
								PrefixCount pc = replay_prefix(fd, pre);
								char lbl[48];
								std::snprintf(lbl, sizeof(lbl),
											  "appr%zu recul %u", a, back);
								if(!pc.ok) {
									std::lock_guard<std::mutex> lk(fmx);
									std::printf("  %-14s !! prefixe non "
												"rejouable (%zu/%zu)\n", lbl,
												pc.used, pre.size());
									continue;
								}
								SearchConfig fcfg = cfg;
								fcfg.time_limit_ms = left;
								fcfg.max_decisions =
									FinisherDepth(cfg.max_decisions, pre.size());
								fcfg.initial_summons = pc.summons;
								fcfg.initial_turns = pc.turns;
								fcfg.initial_resolved = pc.resolved;
								fcfg.max_solutions = opt.optimize ? 12 : 4;
								if(opt.optimize && best_known_burn)
									fcfg.burn_limit = best_known_burn;
								Search fs(fd, fa, start_yrp, fcfg);
								fs.RunLevin(target, plan, merged_policy);
								const SearchStats& st = fs.Stats();
								fin_goal_hits += st.goal_hits;
								fin_burn_cuts += st.burn_cuts;
								std::lock_guard<std::mutex> lk(fmx);
								// Le compteur de re-enracinements est IMPRIME :
								// sans lui, un rerooter qui ne mord jamais est
								// indiscernable d'un rerooter qui ne sert a
								// rien, et l'A/B de la session 7ter a ete lu
								// sans cette colonne (piege 40).
								char rr[64] = "";
								if((cfg.levin_reroot || cfg.reroot_h > 0) &&
								   st.nodes)
									std::snprintf(rr, sizeof(rr),
												  " rr=%llu h0=%.0f%s",
												  (unsigned long long)st.reroots,
												  st.h_root,
												  (st.levin_overflow ||
												   st.lam_saturated)
													  ? " !!NUM" : "");
								char rf[32] = "";
								if(st.levin_children && st.reroots)
									std::snprintf(rf, sizeof(rf), " rr/ar=%.2f",
											  double(st.reroots) /
												  double(st.levin_children));
								char rg[48] = "";
								if(st.recipe_h_count)
									std::snprintf(rg, sizeof(rg),
												  " rec=%llu hR=%.1f",
												  (unsigned long long)
													  st.recipes_seen,
												  st.recipe_h_sum /
													  double(st.recipe_h_count));
								// Taux de rejeu (9.18 (c) : le cout est le
								// REJEU) : aretes rejouees / aretes de chaine,
								// par expansion. Egaux, la pile n'absorbe
								// rien ; 0.0, elle absorbe tout.
								char rj[48] = "";
								if(st.replay_chain && st.nodes)
									std::snprintf(rj, sizeof(rj),
												  " rj=%.1f/%.1f",
												  double(st.replay_decisions) /
													  double(st.nodes),
												  double(st.replay_chain) /
													  double(st.nodes));
								// ARETES MACRO (chantier 3) : enfilees/absorbees/avortees, PAR
								// RACINE. Sans cette colonne, un finisseur ou aucune macro n'est
								// jamais proposable serait indiscernable d'un finisseur ou elles
								// ne servent a rien (piege 52).
								char mc[64] = "";
								if(cfg.finisher_options && (st.macro_taken || st.macro_aborted))
									std::snprintf(mc, sizeof(mc), " mac=%llu/%llu/%llu",
												  (unsigned long long)st.macro_taken,
												  (unsigned long long)st.macro_absorbed,
												  (unsigned long long)st.macro_aborted);
								std::printf("  %-14s %9llu exp. %7.1f s  best "
											"%u/%zu%s%s%s%s%s  b=%llu  %s%s\n", lbl,
											(unsigned long long)st.nodes,
											st.ms / 1000.0, st.best_overlap,
											target.codes.size(), rr, rf, rg, rj, mc,
											(unsigned long long)st.edges_skipped,
											SearchOutcome(st),
											fs.Solutions().empty()
												? "" : "  <-- BUT");
								for(Solution x : fs.Solutions()) {
									std::vector<std::vector<uint8_t>> full =
										pre;
									full.insert(full.end(),
												x.responses.begin(),
												x.responses.end());
									x.responses = std::move(full);
									x.decisions += static_cast<uint32_t>(
										pre.size());
									x.actions += pc.actions;
									AR.sols.push_back(std::move(x));
									found.fetch_add(1);
								}
								// La ligne jointe de la conversion LTS aussi
								// (gabarit verifie) : c'est ICI qu'une
								// approche a rips complets se referme.
								if(same_gabarit(ap)) {
									merge_joint(st, pre);
									// Go-Explore complet (s24) : les cellules
									// du LTS d'approche entrent dans l'archive
									// globale, re-enracinees. Meme garde de
									// gabarit que la ligne jointe.
									if(opt.archive_fin)
										merge_rebased(fs.Archive(), pre);
								}
							}
							fa.Pop();
						} else {
							std::lock_guard<std::mutex> lk(fmx);
							std::printf("  !! duel de l'approche %zu non "
										"initialisable : %s\n", a, err.c_str());
						}
					}
					ReportPoison("finisseur --approach", fa);
					fa.Shutdown();
				});
			}
			for(auto& t : apool)
				t.join();
			prof::PrintPhase("finisseur approches");
		}

		// --- phase A2 : tirages NRPA enracines sur les reculs PROFONDS des
		// approches. Tous les workers, repartis par racine ; meilleure
		// sequence partagee PAR RACINE (des prefixes differents rendent les
		// sequences incompatibles entre racines) ; politique initiale = la
		// politique fusionnee de la phase tirages ; les cartes --resolve
		// portent le biais des indices. C'est la passe qui a une chance de
		// trouver le suffixe entier (rips + refermeture, ~60-150 decisions).
		{
			struct NrpaRoot {
				int64_t a;   // index d'approche ; -1 = duel de DEPART
				uint32_t back;
				std::string label;
				std::vector<std::vector<uint8_t>> pre;
			};
			std::vector<NrpaRoot> roots2;
			char rlbl[48];
			// Etats RIPPES de l'archive (duel de depart) — les racines qui ont
			// deja franchi le verrou : refermer le board depuis elles est la
			// classe de probleme que le moteur sait resoudre. C'est le pont
			// mesure manquant (dizaines de milliers de lignes a 3 rips d'un
			// cote, des 8/8 muets de l'autre, jamais les deux).
			{
				std::vector<const ArchiveEntry*> ripped;
				const ArchiveEntry* deepest = nullptr;
				for(const auto& [cell, e] : global_archive)
					if(e.resolves > 0) {
						ripped.push_back(&e);
						// La cellule la plus RIPPEE est toujours une racine :
						// le score (sp_eff) est domine par les barreaux de
						// board (~40 contre <= 4 de rips), donc le top-3 par
						// score peut n'offrir que des r1 — mesure sur le run
						// diagnostic : les trois racines etaient « 5/6 r1 »
						// alors qu'une cellule r2 existait plus bas. Le max
						// d'un axe MESURE, pas un choix.
						if(!deepest || e.resolves > deepest->resolves ||
						   (e.resolves == deepest->resolves &&
							e.score > deepest->score))
							deepest = &e;
					}
				std::sort(ripped.begin(), ripped.end(),
						  [](const ArchiveEntry* x, const ArchiveEntry* y) {
							  return x->score > y->score;
						  });
				if(ripped.size() > 3)
					ripped.resize(3);
				if(deepest &&
				   std::find(ripped.begin(), ripped.end(), deepest) ==
					   ripped.end())
					ripped.push_back(deepest);
				for(size_t i = 0; i < ripped.size(); ++i) {
					// Reculs DERIVES de la longueur du chemin archive, plus
					// une constante d'etalon ({0,20,40} : sous le point de
					// non-retour du detour, ~60-90 decisions d'amont sur le
					// run diagnostic — 200 k tirages a recul 0 pour zero rip).
					// L/12, L/6, L/3 couvrent la re-preparation d'un rip
					// (~8 %), d'une manoeuvre (~17 %) et d'un tiers de ligne,
					// quelle que soit l'echelle du deck.
					const uint32_t plen =
						static_cast<uint32_t>(ripped[i]->path.size());
					uint32_t prev = ~0u;
					for(uint32_t back : { 0u, plen / 12u, plen / 6u,
										  plen / 3u }) {
						if(back == prev)
							continue;   // chemins courts : reculs confondus
						prev = back;
						if(ripped[i]->path.size() > back) {
							std::snprintf(rlbl, sizeof(rlbl),
										  "rip%zu(%u/%zu r%u) recul %u", i,
										  ripped[i]->overlap,
										  target.codes.size(),
										  ripped[i]->resolves, back);
							roots2.push_back(
								{ -1, back, rlbl,
								  std::vector<std::vector<uint8_t>>(
									  ripped[i]->path.begin(),
									  ripped[i]->path.end() - back) });
						}
					}
				}
			}
			for(size_t a = 0; a < approach_runs.size(); ++a) {
				const Replay* ap = approach_runs[a].holder->IsStreamed()
					? approach_runs[a].holder->Embedded()
					: approach_runs[a].holder.get();
				if(!ap)
					continue;
				// Fenetre mesuree (session 4, approche test 4) : a recul 60
				// la sequence complete de rips n'est plus jouable (0 ligne a
				// 3 rips), a recul 70-80 elle l'est (~4 000 par worker) — la
				// grille couvre le point de non-retour.
				for(uint32_t back : { 60u, 70u, 80u, 90u, 110u, 150u })
					if(ap->responses.size() > back) {
						std::snprintf(rlbl, sizeof(rlbl), "appr%zu recul %u",
									  a, back);
						roots2.push_back(
							{ static_cast<int64_t>(a), back, rlbl,
							  std::vector<std::vector<uint8_t>>(
								  ap->responses.begin(),
								  ap->responses.end() - back) });
					}
			}
			// Optimisation : reculs PROFONDS des solutions les moins cheres
			// (duel de depart) — la restructuration d'une fin de ligne se joue
			// a 60-150 decisions du but, le territoire de l'echantillonnage.
			if(opt.optimize && !sols.empty()) {
				std::vector<const Solution*> cheap;
				cheap.reserve(sols.size());
				for(const Solution& s : sols)
					cheap.push_back(&s);
				std::sort(cheap.begin(), cheap.end(),
						  [](const Solution* x, const Solution* y) {
							  return std::tie(x->burned, x->actions,
											  x->decisions) <
									 std::tie(y->burned, y->actions,
											  y->decisions);
						  });
				for(size_t i = 0; i < cheap.size() && i < 2; ++i)
					for(uint32_t back : { 60u, 90u, 120u, 150u })
						if(cheap[i]->responses.size() > back) {
							std::snprintf(rlbl, sizeof(rlbl),
										  "sol%zu(b%u) recul %u", i,
										  cheap[i]->burned, back);
							roots2.push_back(
								{ -1, back, rlbl,
								  std::vector<std::vector<uint8_t>>(
									  cheap[i]->responses.begin(),
									  cheap[i]->responses.end() - back) });
						}
			}
			if(!roots2.empty() && found.load() < found_stop &&
			   a2_deadline - MsSince(t0) > 2000) {
				std::vector<std::unique_ptr<NrpaShared>> shared2;
				for(size_t i = 0; i < roots2.size(); ++i)
					shared2.push_back(std::make_unique<NrpaShared>());
				uint64_t fseed = opt.seed
					? opt.seed
					: static_cast<uint64_t>(
						  std::chrono::high_resolution_clock::now()
							  .time_since_epoch().count());
				std::vector<std::thread> npool;
				for(unsigned w = 0; w < threads; ++w) {
					npool.emplace_back([&, w] {
						const size_t r = w % roots2.size();
						const NrpaRoot& R = roots2[r];
						// Duel de la racine : l'en-tete de l'approche, ou le
						// duel de DEPART pour les etats rippes de l'archive.
						const Replay* src = &start_yrp;
						if(R.a >= 0) {
							src = approach_runs[static_cast<size_t>(R.a)]
									  .holder->IsStreamed()
								? approach_runs[static_cast<size_t>(R.a)]
									  .holder->Embedded()
								: approach_runs[static_cast<size_t>(R.a)]
									  .holder.get();
							if(!src)
								return;
						}
						Arena fa;
						std::string err;
						if(!fa.Init(opt.arena_mb << 20, 0, err))
							return WorkerAbort("arene (finisseur, racines de recul)", err);
						{
							Duel fd(db, scripts, &fa);
							if(fd.Create(src->seed, src->duel_flags,
										 src->start_lp, src->start_hand,
										 src->draw_count, err) &&
							   fd.Setup(*src, err,
										cons.opp_hand.empty()
											? nullptr : &cons.opp_hand,
										static_cast<uint8_t>(
											1 - opt.target_player))) {
								if(opt.stop_gc)
									fd.SetLuaGc(false);
								double left = a2_deadline - MsSince(t0);
								if(left >= 2000 && found.load() < found_stop) {
									PrefixCount pc = replay_prefix(fd, R.pre);
									if(!pc.ok) {
										std::lock_guard<std::mutex> lk(fmx);
										std::printf("  %-22s !! prefixe non "
													"rejouable (%zu/%zu)\n",
													R.label.c_str(), pc.used,
													R.pre.size());
									} else {
										SearchConfig fcfg = cfg;
										fcfg.time_limit_ms = left;
										fcfg.max_decisions = FinisherDepth(
											cfg.max_decisions, R.pre.size());
										fcfg.initial_summons = pc.summons;
										fcfg.initial_turns = pc.turns;
										fcfg.initial_resolved = pc.resolved;
										fcfg.max_solutions =
											opt.optimize ? 12 : 4;
										if(opt.optimize && best_known_burn)
											fcfg.burn_limit = best_known_burn;
										// Borne partagee, ici aussi : les
										// racines visent le meme board.
										if(opt.optimize && opt.burn_share)
											fcfg.shared_burn = &shared_burn;
										fcfg.nrpa_shared = shared2[r].get();
										fcfg.nrpa_init = &merged_policy;
										fcfg.nrpa_restart_keep =
											static_cast<float>(opt.nrpa_keep);
										Search fs(fd, fa, start_yrp, fcfg);
										fs.RunNrpa(target, plan,
												   fseed +
													   w * 0x9E3779B97F4A7C15ull +
													   1);
										const SearchStats& st = fs.Stats();
										fin_goal_hits += st.goal_hits;
										fin_burn_cuts += st.burn_cuts;
										std::lock_guard<std::mutex> lk(fmx);
										if(opt.probe_repeat) {
											fin_rollouts += st.rollout_count;
											for(int q = 0; q < 4; ++q) {
												const RepeatProbe& s0 = st.rep[q];
												RepeatProbe& a0 = fin_rep[q];
												if(!a0.code)
													a0.code = s0.code;
												for(int k = 0; k < 5; ++k)
													a0.reached[k] += s0.reached[k];
												a0.more_n += s0.more_n;
												a0.more_sum += s0.more_sum;
												a0.rest_sum += s0.rest_sum;
												a0.more_kept += s0.more_kept;
												a0.more_lost += s0.more_lost;
												a0.after_sum += s0.after_sum;
												a0.first_depth_sum +=
													s0.first_depth_sum;
												a0.d0_samples += s0.d0_samples;
												a0.known |= s0.known;
												if(s0.more_n) {
													a0.more_min = (std::min)(
														a0.more_min, s0.more_min);
													a0.more_max = (std::max)(
														a0.more_max, s0.more_max);
												}
												if(s0.d0 != 0xffffffffu &&
												   (a0.d0 == 0xffffffffu ||
													s0.d0 > a0.d0)) {
													a0.d0 = s0.d0;
													a0.rest0 = s0.rest0;
												}
											}
										}
										std::printf(
											"  %-22s w%-2u %8llu tirages %9llu "
											"etats  best %u/%zu  rips "
											"%llu/%llu/%llu  crete-rip %u/%zu%s\n",
											R.label.c_str(), w,
											(unsigned long long)st.rollout_count,
											(unsigned long long)st.nodes,
											st.best_overlap,
											target.codes.size(),
											(unsigned long long)st.resolve_reached[0],
											(unsigned long long)st.resolve_reached[1],
											(unsigned long long)st.resolve_reached[2],
											st.best_overlap_ripped,
											target.codes.size(),
											fs.Solutions().empty()
												? "" : "  <-- BUT");
										for(Solution x : fs.Solutions()) {
											std::vector<std::vector<uint8_t>>
												full = R.pre;
											full.insert(full.end(),
														x.responses.begin(),
														x.responses.end());
											x.responses = std::move(full);
											x.decisions +=
												static_cast<uint32_t>(
													R.pre.size());
											x.actions += pc.actions;
											if(R.a >= 0)
												approach_runs[static_cast<size_t>(
																  R.a)]
													.sols.push_back(
														std::move(x));
											else
												sols.push_back(std::move(x));
											found.fetch_add(1);
										}
										// Une racine du duel de DEPART qui
										// ameliore la crete vaut d'etre
										// conservee (best_approach complet).
										if(R.a < 0 &&
										   st.best_overlap > best_overlap) {
											best_overlap = st.best_overlap;
											best_board = st.best_board;
											best_mzone = st.best_mzone;
											best_szone = st.best_szone;
											best_path = R.pre;
											best_path.insert(
												best_path.end(),
												st.best_path.begin(),
												st.best_path.end());
										}
										// La ligne jointe aussi — depuis le duel
										// de DEPART, ou depuis une approche au
										// MEME gabarit (verifie, pas suppose).
										// Et les cellules de ces tirages
										// enracines (s24, --archive-fin) :
										// c'est la phase qui produit les
										// lignes a rips, exactement celles que
										// l'archive n'a jamais vues.
										if(R.a < 0) {
											merge_joint(st, R.pre);
											if(opt.archive_fin)
												merge_rebased(fs.Archive(),
															  R.pre);
										} else {
											ApproachSols& JAR = approach_runs
												[static_cast<size_t>(R.a)];
											const Replay* asrc =
												JAR.holder->IsStreamed()
													? JAR.holder->Embedded()
													: JAR.holder.get();
											if(same_gabarit(asrc)) {
												merge_joint(st, R.pre);
												if(opt.archive_fin)
													merge_rebased(fs.Archive(),
																  R.pre);
											}
										}
									}
								}
							} else {
								WorkerAbort("duel (finisseur, racines de recul)", err);
							}
						}
						ReportPoison("finisseur, racines de recul", fa);
						fa.Shutdown();
					});
				}
				for(auto& t : npool)
					t.join();
				prof::PrintPhase("tirages A2 (reculs)");
			}
		}

		// --- phase 2 : racines sur le duel de depart.
		unsigned nw = (std::min<unsigned>)(
			threads, static_cast<unsigned>(roots.size()));
		std::vector<std::thread> pool;
		for(unsigned w = 0; w < nw; ++w) {
			pool.emplace_back([&] {
				Arena fa;
				std::string err;
				if(!fa.Init(opt.arena_mb << 20, 0, err))
					return WorkerAbort("arene (finisseur, phase 2)", err);
				{
					Duel fd(db, scripts, &fa);
					if(fd.Create(start_yrp.seed, start_yrp.duel_flags,
								 start_yrp.start_lp, start_yrp.start_hand,
								 start_yrp.draw_count, err) &&
					   fd.Setup(start_yrp, err,
								cons.opp_hand.empty() ? nullptr : &cons.opp_hand,
								static_cast<uint8_t>(1 - opt.target_player))) {
						if(opt.stop_gc)
							fd.SetLuaGc(false);
						// Base des departs de reserve (zone 5) pour la sonde
						// de racine : la RESERVE au depart du duel.
						const uint32_t root_res0 =
							fd.Count(static_cast<uint8_t>(opt.target_player),
									 0x01u) +
							fd.Count(static_cast<uint8_t>(opt.target_player),
									 0x40u);
						fa.Push();   // position de depart du duel
						for(;;) {
							size_t i = next_root.fetch_add(1);
							if(i >= roots.size())
								break;
							double left = budget - MsSince(t0);
							// Quelques solutions suffisent : les racines
							// restantes n'apporteraient que des variantes.
							// (En optimisation : jamais assez — found_stop.)
							if(left < 2000 || found.load() >= found_stop)
								break;
							fa.Restore();
							PrefixCount pc = replay_prefix(fd, roots[i].pre);
							if(!pc.ok) {
								std::lock_guard<std::mutex> lk(fmx);
								std::printf("  %-14s !! prefixe non rejouable "
											"(%zu/%zu)\n",
											roots[i].label.c_str(), pc.used,
											roots[i].pre.size());
								continue;
							}
							// SONDE DE RACINE (s21) : QUELS barreaux cet etat
							// sert — le vecteur empaquete, 4 bits par exigence
							// dans l'ordre du cablage. C'est la reponse a « la
							// conjonction du but existe-t-elle dans une
							// cellule ? », lisible racine par racine.
							char spv[40] = "";
							if(!cfg.serial_reqs.empty()) {
								uint64_t pk = 0;
								SerialProgress(
									fd,
									static_cast<uint8_t>(opt.target_player),
									cfg.serial_reqs, db, root_res0, &pk);
								std::snprintf(spv, sizeof spv, " sp=%013llx",
											  (unsigned long long)pk);
							}
							SearchConfig fcfg = cfg;
							// PLAFOND PAR RACINE, avec restitution (s22,
							// chantier 5). La premiere racine de recul
							// mangeait tout le budget (138-399 s mesures en
							// s21) et les racines suivantes ne tournaient
							// JAMAIS. Chaque racine recoit le restant divise
							// par les racines restantes (plancher 2 s) ; une
							// racine qui s'epuise tot RESTITUE son solde aux
							// suivantes par la re-lecture de `left`. Sous
							// plusieurs workers le denominateur est
							// approximatif — la restitution reste exacte.
							const size_t roots_left =
								roots.size() > i ? roots.size() - i : 1;
							fcfg.time_limit_ms = (std::min)(
								left,
								(std::max)(2000.0,
										   left / double(roots_left)));
							fcfg.max_decisions = FinisherDepth(
								cfg.max_decisions, roots[i].pre.size());
							fcfg.initial_summons = pc.summons;
							fcfg.initial_turns = pc.turns;
							fcfg.initial_resolved = pc.resolved;
							fcfg.max_solutions = opt.optimize ? 12 : 4;
							if(opt.optimize && best_known_burn)
								fcfg.burn_limit = best_known_burn;
							Search fs(fd, fa, start_yrp, fcfg);
							fs.RunLevin(target, plan, merged_policy);
							const SearchStats& st = fs.Stats();
							fin_goal_hits += st.goal_hits;
							fin_burn_cuts += st.burn_cuts;
							std::lock_guard<std::mutex> lk(fmx);
							// Le rerooter est instrumente ICI AUSSI : la correction
							// du piege 52 n'avait ete faite que dans la table
							// d'--approach, donc muette dans le mode but seul
							// SANS --approach, celui que la session 8 venait de
							// construire.
							char rr[64] = "";
							if((cfg.levin_reroot || cfg.reroot_h > 0) && st.nodes)
								std::snprintf(rr, sizeof(rr), " rr=%llu h0=%.0f%s",
											  (unsigned long long)st.reroots,
											  st.h_root,
											  (st.levin_overflow ||
											   st.lam_saturated) ? " !!NUM" : "");
							// FRACTION des aretes qui se re-enracinent. `rr`
							// seul ne separe pas « le rerooter mord parfois »
							// de « il se re-enracine PARTOUT » — deux pannes
							// opposees, et la seconde degenere le cout en
							// mesure locale.
							char rf[32] = "";
							if(st.levin_children && st.reroots)
								std::snprintf(rf, sizeof(rf), " rr/ar=%.2f",
											  double(st.reroots) /
												  double(st.levin_children));
							char rg[48] = "";
							if(st.recipe_h_count)
								std::snprintf(rg, sizeof(rg), " rec=%llu hR=%.1f",
											  (unsigned long long)st.recipes_seen,
											  st.recipe_h_sum /
												  double(st.recipe_h_count));
							// Taux de rejeu — ici aussi (le piege 52 s'etait
							// deja produit sur cette table, pour le rerooter).
							char rj[48] = "";
							if(st.replay_chain && st.nodes)
								std::snprintf(rj, sizeof(rj), " rj=%.1f/%.1f",
											  double(st.replay_decisions) /
												  double(st.nodes),
											  double(st.replay_chain) /
												  double(st.nodes));
							// ARETES MACRO (chantier 3) : enfilees/absorbees/
							// avortees, PAR RACINE. Sans cette colonne, un
							// finisseur ou aucune macro n'est jamais proposable
							// serait indiscernable d'un finisseur ou elles ne
							// servent a rien (piege 52).
							char mc[64] = "";
							if(cfg.finisher_options &&
							   (st.macro_taken || st.macro_aborted))
								std::snprintf(mc, sizeof(mc),
											  " mac=%llu/%llu/%llu",
											  (unsigned long long)st.macro_taken,
											  (unsigned long long)st.macro_absorbed,
											  (unsigned long long)st.macro_aborted);
							std::printf("  %-14s %9llu exp. %7.1f s  best %u/%zu"
										"%s%s%s%s%s%s  b=%llu  %s%s\n",
										roots[i].label.c_str(),
										(unsigned long long)st.nodes,
										st.ms / 1000.0, st.best_overlap,
										target.codes.size(), rr, rf, rg, rj, mc,
										spv,
										(unsigned long long)st.edges_skipped,
										SearchOutcome(st),
										fs.Solutions().empty() ? ""
															   : "  <-- BUT");
							for(Solution x : fs.Solutions()) {
								std::vector<std::vector<uint8_t>> full =
									roots[i].pre;
								full.insert(full.end(), x.responses.begin(),
											x.responses.end());
								x.responses = std::move(full);
								x.decisions += static_cast<uint32_t>(
									roots[i].pre.size());
								x.actions += pc.actions;
								sols.push_back(std::move(x));
								found.fetch_add(1);
							}
							// Une approche amelioree par le finisseur vaut
							// d'etre conservee (best_approach) — chemin
							// COMPLET, prefixe compris.
							if(st.best_overlap > best_overlap) {
								best_overlap = st.best_overlap;
								best_board = st.best_board;
								best_mzone = st.best_mzone;
								best_szone = st.best_szone;
								best_path = roots[i].pre;
								best_path.insert(best_path.end(),
												 st.best_path.begin(),
												 st.best_path.end());
							}
							merge_joint(st, roots[i].pre);
							// Go-Explore complet (s24) : duel de depart —
							// re-enracinement direct, aucun gabarit a verifier.
							if(opt.archive_fin)
								merge_rebased(fs.Archive(), roots[i].pre);
						}
						fa.Pop();
					} else {
						WorkerAbort("duel (finisseur, phase 2)", err);
					}
				}
				ReportPoison("finisseur, phase 2", fa);
				fa.Shutdown();
			});
		}
		for(auto& t : pool)
			t.join();
		prof::PrintPhase("finisseur duel de depart");
		// La vie de la fusion (s24, --archive-fin) : TOUJOURS dite quand le
		// mecanisme est arme, y compris « +0 » — un finisseur qui n'apporte
		// aucune cellule est une information (archives vides ? gabarit
		// refuse ?), pas un silence.
		if(opt.archive_fin)
			std::printf("  ARCHIVE DU FINISSEUR (s24) : +%zu cellule(s) "
						"nouvelle(s), %zu amelioree(s) — archive globale %zu\n",
						fin_cells_new, fin_cells_upd, global_archive.size());
		// SONDE DE REPETITION cote FINISSEUR : les tirages ENRACINES sont
		// invisibles dans la table de la phase tirages, et c'est justement la
		// que la session 14 avait trouve sa 2e Liger (9.21 (d)). Sans cette
		// seconde table, un « jamais » se lirait comme un jamais du RUN.
		if(opt.probe_repeat && fin_rollouts)
			PrintRepeatProbe(fin_rep, fin_rollouts, db,
							 true, true,
							 "tirages ENRACINES du finisseur");
		// Session 6 : le bilan de la borne B&B du finisseur.
		if(opt.optimize)
			std::printf("  finisseur : %llu atteinte(s) du but, %llu coupure(s) "
						"borne brulees%s\n",
						(unsigned long long)fin_goal_hits.load(),
						(unsigned long long)fin_burn_cuts.load(),
						opt.burn_share ? "" : "  [partage OFF]");
	};

	// En optimisation, le finisseur tourne MEME quand les tirages ont des
	// lignes : les prefixes des solutions les moins cheres sont ses racines,
	// et un recul qui economise une brulee est une victoire.
	if((sols.empty() || opt.optimize) &&
	   (spent < opt.solve_ms || opt.finisher_min > 0) &&
	   (!best_path.empty() || !global_archive.empty() ||
		!opt.approach_files.empty() || !sols.empty())) {
		double budget = opt.finisher_min > 0
			? (std::max)(opt.finisher_min, (opt.solve_ms - spent) * 0.8)
			: (std::min)((opt.solve_ms - spent) * 0.8, 240000.0);
		std::printf("  budget finisseur : %.0f s sur %.0f s restantes (0,8x%s)\n",
					budget / 1000.0, (opt.solve_ms - spent) / 1000.0,
					opt.finisher_min > 0 ? ", plancher --finisher-min"
										 : ", plafond 240 s");
		auto t0 = Clock::now();
		if(opt.finisher == "mono") {
			if(!best_path.empty())
				run_mono(budget);
		} else if(opt.finisher == "ab") {
			// La mesure : les deux moteurs, budget egal, memes racines de
			// depart (le mono n'en connait qu'une — c'est precisement ce qui
			// est mesure).
			size_t before = sols.size();
			if(!best_path.empty())
				run_mono(budget / 2);
			std::printf("  [A/B] mono : %zu solution(s)\n", sols.size() - before);
			before = sols.size();
			run_levin(budget / 2);
			std::printf("  [A/B] levin : %zu solution(s)\n", sols.size() - before);
		} else {
			run_levin(budget);
		}
		spent += MsSince(t0);
		if(!sols.empty())
			std::printf("  %zu ligne(s) atteignant le board via le finisseur.\n",
						sols.size());
	}

	// Solutions des racines d'approche : ecrites contre l'en-tete de LEUR
	// fichier (cf. ApproachSols), verification avant ecriture comprise — sur
	// le duel de l'approche, augmente du meme --opp-hand.
	bool approach_found = false;
	for(ApproachSols& AR : approach_runs) {
		if(AR.sols.empty())
			continue;
		approach_found = true;
		std::sort(AR.sols.begin(), AR.sols.end(),
				  [](const Solution& x, const Solution& y) {
					  if(x.burned != y.burned) return x.burned < y.burned;
					  if(x.actions != y.actions) return x.actions < y.actions;
					  return x.decisions < y.decisions;
				  });
		const Replay* ap = AR.holder->IsStreamed() ? AR.holder->Embedded()
												   : AR.holder.get();
		std::printf("\n  %zu ligne(s) via l'approche %s\n"
					"  (rejouables sur CE fichier, avec le meme --opp-hand) :\n",
					AR.sols.size(), AR.file.c_str());
		std::printf("  %-4s %10s %9s %11s\n", "#", "brulees", "actions",
					"decisions");
		for(size_t i = 0; i < AR.sols.size() && i < 8; ++i)
			std::printf("  %-4zu %10u %9u %11u\n", i, AR.sols[i].burned,
						AR.sols[i].actions, AR.sols[i].decisions);
		WriteSolutions(AR.sols, *ap, target, opt, db, scripts, opt.outdir,
					   cons, cons.opp_hand.empty() ? nullptr : &cons.opp_hand);
	}

	if(!sols.empty()) {
		// Dedup par chemin : les workers anytime convergent souvent sur la
		// meme ligne — l'ecrire seize fois n'apporte rien.
		{
			std::unordered_set<uint64_t> seen;
			std::vector<Solution> uniq;
			uniq.reserve(sols.size());
			for(Solution& s : sols) {
				uint64_t h = 1469598103934665603ull;
				for(const auto& r : s.responses) {
					for(uint8_t b : r) {
						h ^= b;
						h *= 1099511628211ull;
					}
					h ^= 0xff;
					h *= 1099511628211ull;
				}
				if(seen.insert(h).second)
					uniq.push_back(std::move(s));
			}
			if(uniq.size() < sols.size())
				std::printf("\n  %zu ligne(s) distinctes (%zu doublons de "
							"chemin fusionnes)\n", uniq.size(),
							sols.size() - uniq.size());
			sols = std::move(uniq);
		}
		std::sort(sols.begin(), sols.end(), [](const Solution& a, const Solution& b) {
			if(a.burned != b.burned) return a.burned < b.burned;
			if(a.actions != b.actions) return a.actions < b.actions;
			return a.decisions < b.decisions;
		});
		std::printf("\n  %-4s %10s %9s %11s %8s %8s %8s\n", "#", "brulees",
					"actions", "decisions", "main", "deck", "extra");
		for(size_t i = 0; i < sols.size() && i < 10; ++i) {
			const Solution& x = sols[i];
			std::printf("  %-4zu %10u %9u %11u %8u %8u %8u\n", i, x.burned,
						x.actions, x.decisions, x.hand_left, x.deck_left,
						x.extra_left);
		}
		std::printf("\n  %-4s %10u %9u %11zu   (reference, sur son propre deck)\n",
					"ref", ref_burned, ref_actions, ref_decisions);
		WriteSolutions(sols, start_yrp, target, opt, db, scripts, opt.outdir,
					   cons, cons.opp_hand.empty() ? nullptr : &cons.opp_hand);
		arena.Restore();
		return;
	}
	if(approach_found) {
		// Les lignes d'approche repondent a la question posee ; la passe a
		// ecarts bornes n'y ajouterait que des variantes.
		arena.Restore();
		return;
	}

	std::printf("\n--- recherche a ecarts bornes autour du plan ---\n");
	std::printf("  workers        : %u,  profondeur max %u decisions,  "
				"nouveaute %s (patience %u, stricte)\n", threads,
				cfg.max_decisions, patience ? "active" : "desactivee", patience);
	std::printf("\n  %-8s %10s %12s %11s %10s %9s\n", "ecarts", "solutions",
				"etats", "transpos.", "coupures", "duree");

	uint32_t reached = 0;
	for(uint32_t k = 0; k <= 12 && spent < opt.solve_ms && sols.empty(); ++k) {
		reached = k;
		double budget = opt.solve_ms - spent;
		auto t0 = Clock::now();
		unsigned n = (k == 0) ? 1u : threads;
		// Un jeton par ETAT DISTINCT au niveau de reclamation — pas par etape de
		// plan : en mode but seul le plan est vide, et la table d'avant
		// n'offrait alors qu'une seule case pour seize workers (C1).
		//
		// Dimensionnement genereux (65 536 cases, 512 Ko) : le nombre de points
		// de reclamation n'est pas connu d'avance et se compte en milliers
		// (chaque noeud du prefixe ouvre une dizaine de deviations). A
		// saturation la table n'interdit rien, mais elle cesse de PARTITIONNER
		// — les workers se remettent a refaire le meme travail — et c'est ce
		// que dit `Overflow()`, imprime plus bas.
		ClaimTable claims(16384);
		// Table de transposition partagee de la passe (lazy SMP).
		std::unique_ptr<SharedTT> stt;
		if(opt.tt_mb && n > 1)
			stt = std::make_unique<SharedTT>(opt.tt_mb);
		std::mutex merge;
		std::vector<Solution> found;
		uint64_t nodes = 0, transpos = 0, cuts = 0;
		CutCounts cut;
		bool timed_out = false;

		auto worker = [&](unsigned) {
			// Chaque worker a son arene et son duel — et ce duel est monte sur le
			// replay de DEPART, pas sur la reference.
			// (best_overlap / best_monsters remontent par `merge`.)
			Arena local_arena;
			std::string err;
			if(!local_arena.Init(opt.arena_mb << 20, 0, err))
				return WorkerAbort("arene (transplantation)", err);
			{
				Duel local(db, scripts, &local_arena);
				if(local.Create(start_yrp.seed, start_yrp.duel_flags,
								start_yrp.start_lp, start_yrp.start_hand,
								start_yrp.draw_count, err) &&
				   local.Setup(start_yrp, err,
							   cons.opp_hand.empty() ? nullptr : &cons.opp_hand,
							   static_cast<uint8_t>(1 - opt.target_player))) {
					if(opt.stop_gc)
						local.SetLuaGc(false);
					SearchConfig wcfg = cfg;
					wcfg.time_limit_ms = budget;
					wcfg.novelty_patience = patience;
					// Strict : seuls les faits jamais vus comptent. La passe a
					// ecarts bornes cherche du MATERIEL neuf, pas des variantes ;
					// et en depth-aware son DFS (deviations profondes d'abord)
					// rendait toute branche plus courte "nouvelle" — 3 584
					// coupures sur 1 M d'etats, un elagage de facade.
					wcfg.novelty_strict = true;
					wcfg.trace = opt.verbose && k == 0;
					// La trace lit les labels ; les chemins chauds ne les
					// construisent plus par defaut.
					wcfg.enumeration.labels = wcfg.trace;
					wcfg.shared_tt = stt.get();
					if(n > 1) {
						wcfg.claim_level = (k <= 1) ? 0u : 1u;
						wcfg.claims = &claims;
					}
					Search s(local, local_arena, start_yrp, wcfg);
					s.RunTransplant(target, plan, k);
					std::lock_guard<std::mutex> lock(merge);
					for(const auto& x : s.Solutions())
						found.push_back(x);
					nodes += s.Stats().nodes;
					transpos += s.Stats().transpositions;
					cuts += s.Stats().novelty_cuts;
					cut.Add(s.Stats());
					timed_out |= s.Stats().hit_time_limit;
					if(s.Stats().best_overlap > best_overlap) {
						best_board = s.Stats().best_board;
						best_path = s.Stats().best_path;
						best_mzone = s.Stats().best_mzone;
						best_szone = s.Stats().best_szone;
					}
					best_overlap = (std::max)(best_overlap, s.Stats().best_overlap);
					best_monsters = (std::max)(best_monsters, s.Stats().best_monsters);
					merge_joint(s.Stats(), {});
				} else {
					std::lock_guard<std::mutex> lock(merge);
					std::printf("  !! duel de depart non initialisable : %s\n",
								err.c_str());
				}
			}
			ReportPoison("transplantation", local_arena);
			local_arena.Shutdown();
		};

		std::vector<std::thread> pool;
		for(unsigned i = 0; i < n; ++i)
			pool.emplace_back(worker, i);
		for(auto& t : pool)
			t.join();
		if(prof::enabled) {
			char lbl[40];
			std::snprintf(lbl, sizeof(lbl), "ecarts plan k=%u", k);
			prof::PrintPhase(lbl);
		}

		double ms = MsSince(t0);
		spent += ms;
		std::printf("  %-8u %10zu %12llu %11llu %10llu %8.1f s   %u/%zu %u mon.%s\n",
					k, found.size(), (unsigned long long)nodes,
					(unsigned long long)transpos, (unsigned long long)cuts,
					ms / 1000.0, best_overlap,
					target.codes.size(), best_monsters,
					timed_out ? "  (budget epuise)" : "");
		PrintCuts(cut);
		if(claims.Overflow())
			std::printf("           !! partition saturee %llu fois : les workers "
						"refont le meme travail\n",
						(unsigned long long)claims.Overflow());
		for(const auto& x : found)
			sols.push_back(x);
	}
	arena.Restore();

	if(sols.empty()) {
		std::printf("\n  AUCUNE ligne trouvee jusqu'a %u ecart(s).\n", reached);
		std::printf("  Meilleure approche : %u des %zu cartes du board reunies, "
					"%u monstre(s) poses.\n", best_overlap, target.codes.size(),
					best_monsters);
		if(best_monsters < 2)
			std::printf("  Cette main ne pose presque rien : le blocage est a "
						"l'ouverture, pas\n  dans la profondeur de recherche.\n");
		ReportBestBoard(best_board, target, db);

		// Tous les codes y sont mais le but ne se declenche pas : le
		// differentiel est un DETAIL — position, materiaux ou compteurs. On
		// l'affiche en face de la cible, c'est la seule information sur
		// laquelle on puisse agir.
		if(best_overlap == target.codes.size() && !best_mzone.empty()) {
			auto dump = [&](const char* label,
							const std::vector<QueriedCard>& zone) {
				for(const auto& c : zone) {
					if(!c.present)
						continue;
					std::string extra;
					if(!c.overlay.empty())
						extra += " +" + std::to_string(c.overlay.size()) + " mat";
					if(!c.counters.empty())
						extra += " +compteurs";
					std::printf("      %s %9u  %-36.36s %-8s%s\n", label,
								c.Code(), db.Name(c.Code()).c_str(),
								PosName(c.position), extra.c_str());
				}
			};
			std::printf("\n  tous les codes y sont : le but ne differe que par "
						"le DETAIL.\n  meilleur etat :\n");
			dump("MZONE", best_mzone);
			dump("SZONE", best_szone);
			std::printf("  cible :\n");
			// LA VRAIE CIBLE, pas celle du gabarit. Une zone S/T vide s'y lit
			// alors comme ce qu'elle est : une exigence d'ABSENCE, que
			// --target-subset leve.
			if(posed)
				std::printf("      (cible POSEE : une zone S/T absente ci-dessous est \n"
							"      EXIGEE VIDE, sauf sous --target-subset)\n");
			dump("MZONE", posed ? posed_mz : ref.target_self.mzone);
			dump("SZONE", posed ? posed_sz : ref.target_self.szone);
		}
		// La meilleure approche merite d'etre CONSERVEE : rejouable dans
		// EDOPro, jugeable, et reprenable comme repertoire d'une prochaine
		// session. Ce n'est PAS une solution — le nom le dit.
		if(!best_path.empty()) {
			std::error_code ec;
			std::filesystem::create_directories(opt.outdir, ec);
			char name[64];
			std::snprintf(name, sizeof(name), "best_approach_%uof%zu.yrp",
						  best_overlap, target.codes.size());
			std::string apath = opt.outdir + "/" + name;
			std::string werr;
			if(WriteYrp1(apath, start_yrp, best_path, werr))
				std::printf("\n  meilleure approche ecrite : %s (%zu decisions, "
							"PAS une solution)\n", apath.c_str(),
							best_path.size());
			else
				std::printf("  !! %s\n", werr.c_str());
		}
		// La meilleure ligne JOINTE (s23) : rips d'abord, board ensuite.
		// Reinjectable par --approach (l'instrument d'isolation s21 : la
		// mecanique convertit quand elle est proche, 3/3 mesure) — c'est le
		// pont entre « des lignes a rips complets » et « un board depuis
		// elles », que les compteurs seuls laissaient mourir avec le run.
		if(best_joint_rp && !best_joint_path.empty()) {
			std::error_code ec;
			std::filesystem::create_directories(opt.outdir, ec);
			char name[64];
			std::snprintf(name, sizeof(name), "best_joint_%ur_%uof%zu.yrp",
						  best_joint_rp, best_joint_overlap,
						  target.codes.size());
			std::string jpath = opt.outdir + "/" + name;
			std::string werr;
			if(WriteYrp1(jpath, start_yrp, best_joint_path, werr)) {
				std::printf("  meilleure ligne JOINTE ecrite : %s (%u rip(s), "
							"%u/%zu au board, %zu decisions, PAS une "
							"solution)\n",
							jpath.c_str(), best_joint_rp, best_joint_overlap,
							target.codes.size(), best_joint_path.size());
				if(outres)
					outres->joint_file = jpath;
			} else
				std::printf("  !! %s\n", werr.c_str());
		}
		if(outres) {
			// Les solutions des racines d'approche comptent : une conversion
			// venue de la ligne reinjectee doit arreter la boucle interne.
			outres->solutions = sols.size();
			for(const ApproachSols& AR : approach_runs)
				outres->solutions += AR.sols.size();
			outres->best_overlap = best_overlap;
			outres->joint_rp = best_joint_rp;
			outres->joint_overlap = best_joint_overlap;
		}
		// Le TRANSPORT (s24, --carry) : tout ce que ce round a appris —
		// archive globale (finisseur compris sous --archive-fin) et politique
		// fusionnee — survit a l'appel pour le round suivant.
		if(carry && opt.carry) {
			carry->archive = std::move(global_archive);
			carry->policy = std::move(merged_policy);
			carry->policy_workers = policy_workers;
		}
		return;
	}

	std::sort(sols.begin(), sols.end(), [](const Solution& a, const Solution& b) {
		if(a.burned != b.burned) return a.burned < b.burned;
		if(a.actions != b.actions) return a.actions < b.actions;
		return a.decisions < b.decisions;
	});
	std::printf("\n  %zu ligne(s) atteignant le board depuis ce deck.\n\n", sols.size());
	std::printf("  %-4s %10s %9s %11s %8s %8s %8s\n", "#", "brulees", "actions",
				"decisions", "main", "deck", "extra");
	for(size_t i = 0; i < sols.size() && i < 10; ++i) {
		const Solution& x = sols[i];
		std::printf("  %-4zu %10u %9u %11u %8u %8u %8u\n", i, x.burned, x.actions,
					x.decisions, x.hand_left, x.deck_left, x.extra_left);
	}
	std::printf("\n  %-4s %10u %9u %11zu   (reference, sur son propre deck)\n", "ref",
				ref_burned, ref_actions, ref_decisions);
	WriteSolutions(sols, start_yrp, target, opt, db, scripts, opt.outdir,
				   cons, cons.opp_hand.empty() ? nullptr : &cons.opp_hand);
}

// JALON 0b — la mesure qui decide de tout.
//
// La taille de l'arbre d'actions (10^97) ne dit rien de la faisabilite : ce qui
// compte est le nombre d'etats DISTINCTS, apres fusion des chemins qui
// convergent. Personne ne peut le deviner : il depend de la structure du deck.
// On le mesure en developpant exhaustivement le graphe a profondeur croissante
// et en observant la courbe.
void RunGrowthMeasurement(Duel& duel, const Replay& yrp, const Options& opt,
						  Arena& arena, const LineResult& ref) {
	std::printf("\n=== courbe de croissance du graphe d'etats ===\n");
	std::printf("  Le nombre d'etats distincts par profondeur decide si\n"
				"  \"exhaustif\" est realiste. C'est une mesure, pas une estimation.\n\n");

	// Board cible : celui de la fin du tour de reference.
	while(arena.Depth() > 1)
		arena.Pop();
	if(arena.Depth() == 0)
		arena.Push();
	arena.Restore();

	// On rejoue la ligne pour capturer la cle du board cible, puis on revient.
	BoardKey target;
	{
		size_t at = 0;
		at += Advance(duel, yrp, at, ref.target_at);
		target = ComputeBoardKey(duel, static_cast<uint8_t>(opt.target_player));
		arena.Restore();
	}
	std::printf("  board cible : %zu cartes, empreinte %016llx\n\n",
				target.entries.size(), (unsigned long long)target.hash);

	// BOARDS ET NON ETATS (session 17). `etats` est ce que la transposition
	// distingue (`StateDigest` : zones + charge utile du prompt, donc les
	// compteurs « une fois par tour ») ; les trois colonnes suivantes comptent
	// des BOARDS, de plus en plus grossiers, tous indifferents a la position et
	// a la colonne :
	//   b.exact = (zone, code, face, materiaux, compteurs)
	//   b.lache = (zone, code, face)
	//   b.codes = les codes seuls
	// Le rapport etats/b.exact est le PRIX de la finesse de la cle.
	// `et.idle` / `b.idle` : les memes, restreints aux points STABLES (prompt
	// idle). C'est LEUR rapport qui mesure le prix de la cle de transposition —
	// ailleurs on est au milieu d'une resolution et deux etats de meme board
	// sont legitimement distincts.
	std::printf("  %-6s %10s %8s %8s %9s %8s %9s %7s\n", "prof.", "etats",
				"b.exact", "b.codes", "et.idle", "b.idle", "duree", "statut");
	uint64_t prev = 0;
	for(uint32_t depth = 2; depth <= opt.growth_max; depth += 2) {
		SearchConfig cfg;
		ApplyMechanisms(opt, cfg);   // chantier D : UN SEUL POINT DE CABLAGE
		cfg.max_decisions = depth;
		cfg.time_limit_ms = opt.growth_ms;
		cfg.max_nodes = 5000000;
		cfg.enumeration.dedup_by_code = true;
		cfg.enumeration.max_subsets = opt.max_subsets;
		// La question de l'operateur : combien de BOARDS, pas combien d'etats.
		cfg.count_boards = true;
		// `--elide-forced` etait cable ICI et seulement ici — c'est ce qui a
		// fait mesurer trois sessions de bancs sur un chemin de recherche ou il
		// ne faisait rien (9.28 (f)). Il vient desormais d'`ApplyMechanisms`,
		// comme partout ailleurs. `--growth` reste le meilleur endroit pour LE
		// MESURER (il rend la profondeur atteinte a budget egal), pas pour le
		// cabler.
		if(depth == 2)   // une fois, au premier palier de profondeur
			ReportMechanisms(cfg, "growth");

		Search search(duel, arena, yrp, cfg);
		search.Run(target);
		arena.Restore();

		const SearchStats& s = search.Stats();
		const char* status = s.hit_time_limit ? "temps"
							 : s.hit_node_limit ? "noeuds" : "epuise";
		std::printf("  %-6u %10llu %8zu %8zu %9llu %8zu %6.0f ms %7s",
					depth, (unsigned long long)s.nodes, s.boards_entries,
					s.boards_codes, (unsigned long long)s.states_idle,
					s.boards_idle, s.ms, status);
		if(prev)
			std::printf("   x%.1f", double(s.nodes) / double(prev));
		std::printf("\n");
		prev = s.nodes;
		if(depth == opt.growth_max || s.hit_time_limit || s.hit_node_limit) {
			// Profil par profondeur : montre ou l'exploration s'arrete
			// reellement, et donc si la saturation vient d'un espace clos ou
			// d'une borne qui mord.
			std::printf("\n  profil du dernier passage (etats nouveaux par "
						"profondeur) :\n   ");
			for(size_t i = 0; i < s.distinct_by_depth.size(); ++i)
				if(s.distinct_by_depth[i])
					std::printf(" %zu:%llu", i,
								(unsigned long long)s.distinct_by_depth[i]);
			// FACTEUR DE FUSION REEL de la table de transposition :
			// expansions / distincts, par profondeur. `expansions_by_depth`
			// etait dimensionne et incremente a quatre endroits, et n'etait lu
			// nulle part (1.6) — or c'est le seul chiffre qui dit si la table
			// fusionne quelque chose la ou l'espace explose.
			std::printf("\n  fusion par profondeur (expansions / distincts) :\n   ");
			for(size_t i = 0; i < s.expansions_by_depth.size() &&
							  i < s.distinct_by_depth.size(); ++i)
				if(s.distinct_by_depth[i])
					std::printf(" %zu:x%.1f", i,
								double(s.expansions_by_depth[i]) /
									double(s.distinct_by_depth[i]));
			std::printf("\n  terminaux : %llu   impasses : %llu\n",
						(unsigned long long)s.terminals,
						(unsigned long long)s.dead_ends);
			// ATTRIBUTION DE LA CLE DE TRANSPOSITION, aux points STABLES.
			// Chaque ligne ajoute une composante a la precedente : l'ecart entre
			// deux lignes consecutives EST le prix de la composante ajoutee.
			// Sans cette lecture, corriger la cle serait un pari.
			if(s.d_full) {
				std::printf("\n  attribution de la cle (valeurs distinctes aux "
							"points idle) :\n");
				std::printf("    board (BoardKey)                    %8zu   "
							"x1.0\n", s.boards_idle);
				auto rap = [&](size_t v) {
					return s.boards_idle ? double(v) / double(s.boards_idle)
										 : 0.0;
				};
				std::printf("    + etat de jeu, COLONNES CONFONDUES  %8zu   "
							"x%.1f\n", s.d_zsort, rap(s.d_zsort));
				std::printf("    + la COLONNE distingue              %8zu   "
							"x%.1f   <== prix de la colonne : x%.2f\n",
							s.d_zones, rap(s.d_zones),
							s.d_zsort ? double(s.d_zones) / double(s.d_zsort)
									  : 0.0);
				std::printf("    + charge utile du prompt            %8zu   "
							"x%.1f\n", s.d_payload, rap(s.d_payload));
				std::printf("    + etat du processeur (= cle reelle) %8zu   "
							"x%.1f\n", s.d_full, rap(s.d_full));
				std::printf("    (le processeur SEUL vaut %zu valeurs "
							"distinctes)\n", s.d_proc);
			}
			// ATTRIBUTION GLOBALE : la repartition de TOUS les noeuds. Sans
			// elle, on corrige la cle sur la foi d'un comptage restreint aux
			// points idle — et une attribution sur un sous-ensemble ne se
			// transporte pas a l'ensemble (lecon de la session 17).
			const uint64_t tot = s.nodes_forced + s.nodes_idle + s.nodes_multi;
			if(tot) {
				auto pc = [&](uint64_t v) { return 100.0 * double(v) / double(tot); };
				std::printf("\n  repartition des noeuds developpes :\n"
							"    FORCES (une seule reponse legale) %10llu   %5.1f %%\n"
							"    idle   (point de decision stable) %10llu   %5.1f %%\n"
							"    autres (selections, chaines)      %10llu   %5.1f %%\n",
							(unsigned long long)s.nodes_forced, pc(s.nodes_forced),
							(unsigned long long)s.nodes_idle, pc(s.nodes_idle),
							(unsigned long long)s.nodes_multi, pc(s.nodes_multi));
				if(s.elided)
					std::printf("    dont JOUES EN LIGNE (--elide-forced) %7llu\n",
								(unsigned long long)s.elided);
			}
		}
		if(s.hit_time_limit || s.hit_node_limit) {
			std::printf("\n  Arret : le budget est atteint des la profondeur %u,\n"
						"  tres loin des %u decisions de la ligne de reference.\n",
						depth, (unsigned)ref.responses_used);
			break;
		}
	}
	std::printf("\n  Lecture : le facteur de croissance par palier est ce qui\n"
				"  determine la profondeur atteignable. Un facteur stable de k\n"
				"  signifie que chaque paire de decisions supplementaire multiplie\n"
				"  le travail par k.\n");
}

} // namespace

int main(int argc, char** argv) {
	// Sans cela, un plantage emporte la fin du tampon et masque l'endroit exact.
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	Options opt;
	if(!ParseArgs(argc, argv, opt)) {
		Usage();
		return 2;
	}
	// Avant la creation du moindre thread : la publication du drapeau passe par
	// le lancement des workers.
	if(opt.profile)
		prof::Enable();
	// SONDE DE REPETITION : elle mesure des DISTANCES sur le graphe de recettes.
	// L'allumer sans graphe ne rendrait qu'un histogramme, muet sur la seule
	// question posee — la famille exacte du « mecanisme silencieusement absent
	// du chemin ». L'implication est appliquee ICI (apres toute la ligne de
	// commande, donc insensible a l'ordre des drapeaux) et elle est DITE.
	if(opt.probe_repeat && opt.recipes < 0) {
		opt.recipes = 0.0;
		std::printf("  --probe-repeat implique --recipes 0 : la sonde mesure des "
					"distances sur le graphe de recettes\n");
	}
	// MEME RAISON pour les trois leviers de la session 17 qui lisent le graphe :
	// sans lui ils s'executent, ne coutent rien et ne font RIEN — un bras d'A/B
	// indiscernable de son temoin, et une conclusion fausse au bout.
	// `--recipes 0` alimente et mesure le graphe SANS l'introduire dans le `h`
	// du finisseur : les mecanismes de la session 17 restent donc les seuls
	// facteurs modifies.
	if((opt.assign || opt.backward) &&
	   opt.recipes < 0) {
		opt.recipes = 0.0;
		std::printf("  --assign / --recipe-w / --backward impliquent --recipes 0 : "
					"les trois lisent le graphe de recettes\n");
	}
	std::string error;
	if(!opt.deck_file.empty() && !opt.start_replay.empty()) {
		std::printf("!! --deck et --start sont exclusifs : l'un construit la "
					"position de depart, l'autre la lit\n");
		return 2;
	}

	Replay replay;
	if(!replay.Load(opt.replay, error)) {
		std::printf("!! %s\n", error.c_str());
		return 1;
	}
	// Un yrpX enveloppe un yrp1 ; un yrp1 se suffit a lui-meme — c'est le format
	// que produit WriteSolutions, et il doit pouvoir revenir en entree.
	const Replay* yrp = replay.IsStreamed() ? replay.Embedded() : &replay;
	if(!yrp) {
		std::printf("!! aucun yrp1 embarque dans %s : les decisions du joueur "
					"sont irrecuperables.\n   Le solveur exige un replay "
					"exportable.\n", opt.replay.c_str());
		return 1;
	}

	// Replay de depart : on n'en garde que le yrp1, seul porteur des decks, de
	// la graine et des parametres de duel. Son flux et ses reponses ne servent
	// a rien ici — c'est une position de depart, pas une ligne a suivre.
	Replay start_replay;
	const Replay* start_yrp = nullptr;
	if(!opt.start_replay.empty()) {
		if(!start_replay.Load(opt.start_replay, error)) {
			std::printf("!! replay de depart : %s\n", error.c_str());
			return 1;
		}
		start_yrp = start_replay.IsStreamed() ? start_replay.Embedded()
											  : &start_replay;
		if(!start_yrp) {
			std::printf("!! aucun yrp1 embarque dans %s : deck et graine "
						"illisibles.\n", opt.start_replay.c_str());
			return 1;
		}
		if(start_yrp->decks.size() <= static_cast<size_t>(opt.target_player)) {
			std::printf("!! le replay de depart ne porte pas de deck pour le "
						"joueur %d.\n", opt.target_player);
			return 1;
		}
	}

	std::printf("=== chargement ===\n");
	auto t_db = Clock::now();
	CardDB db;
	if(!db.Load(opt.workdir, error)) {
		std::printf("!! %s\n", error.c_str());
		return 1;
	}
	double ms_db = MsSince(t_db);
	std::printf("  cartes            : %zu depuis %zu base(s)  (%.0f ms)\n",
				db.Size(), db.Sources().size(), ms_db);

	// Les contraintes se resolvent des que la base est la : une carte
	// introuvable ou ambigue doit arreter AVANT toute recherche.
	LineConstraints cons;
	if(!ResolveConstraints(opt, db, cons))
		return 2;
	// Deux rerooters actifs a la fois ne se composent pas dans notre cout : le
	// second ecraserait le premier en silence. L'article combine les siens par
	// une somme PONDEREE (Th. 3.2), pas par un « et » implicite.
	if(opt.levin_reroot && opt.reroot_h > 0) {
		std::printf("!! --reroot et --reroot-h sont exclusifs (rerooter dur "
					"contre rerooter doux).\n");
		return 2;
	}
	// --no-ref promet que la reference ne sert plus qu'a fournir les parametres
	// du duel. Sans --target elle fournirait encore le BOARD CIBLE, et la
	// promesse serait fausse : on refuse plutot que de mentir dans le rapport.
	if(opt.no_ref && !cons.target_scratch) {
		std::printf("!! --no-ref exige --target : sans lui le board cible vient "
					"encore de la reference.\n");
		return 2;
	}
	if(opt.no_ref && opt.deck_file.empty() && opt.start_replay.empty()) {
		std::printf("!! --no-ref exige --deck (ou --start) : le replay "
					"positionnel n'est plus qu'un gabarit.\n");
		return 2;
	}

	if(opt.mp1_only)
		std::printf("  contrainte : combo en MAIN PHASE 1 seule — l'entree en "
					"Battle Phase (donc la Main 2) est retiree de "
					"l'enumeration ; -> End Phase reste.\n");
	ScriptProvider scripts;
	scripts.Init(opt.workdir, opt.scriptdirs);
	std::printf("  dossiers scripts  : %zu%s\n", scripts.Dirs().size(),
				opt.scriptdirs.empty() ? "" : "  (override)");

	// --no-self-negate (s22ter) : les effets de NEGATION du deck se derivent
	// ICI, au seul endroit ou base, scripts et decks coexistent avant tous
	// les modes. Codes = deck du joueur cible du gabarit, plus la decklist
	// --deck si donnee (surensemble : une paire sans carte ne matche rien).
	// Categories lues dans constant.lua ; zero nom de carte compile.
	if(opt.no_self_negate) {
		std::vector<uint32_t> sn_codes;
		const int tp0 = opt.target_player;
		if(yrp && tp0 >= 0 &&
		   static_cast<size_t>(tp0) < yrp->decks.size())
			for(const auto* l :
				{ &yrp->decks[tp0].main, &yrp->decks[tp0].extra })
				for(uint32_t c : *l)
					sn_codes.push_back(c);
		if(!opt.deck_file.empty()) {
			Deck ydk;
			std::string derr;
			if(LoadYdk(opt.deck_file, ydk, derr))
				for(const auto* l : { &ydk.main, &ydk.extra })
					for(uint32_t c : *l)
						sn_codes.push_back(c);
		}
		ConstantTable snkt;
		uint64_t c_negate = 0, c_disable = 0;
		if(!snkt.Load(scripts) ||
		   (!snkt.Lookup("CATEGORY_NEGATE", c_negate) &
			!snkt.Lookup("CATEGORY_DISABLE", c_disable))) {
			std::printf("!! --no-self-negate : constantes illisibles "
						"(constant.lua absent ?) — discipline ETEINTE, dit "
						"plutot que tue.\n");
		} else if(!sn_codes.empty()) {
			OperatorTable sntbl;
			sntbl.Build(db, scripts, snkt, sn_codes);
			std::string listing;
			for(const auto& [code, co] : sntbl.All())
				for(const DeclaredEffect& e : co.operators) {
					if(!e.at_init ||
					   !(e.category & (c_negate | c_disable)))
						continue;
					const uint32_t cc = db.Canonical(code);
					cons.self_negate.emplace_back(
						cc, e.has_desc ? e.desc_value : 0ull);
					listing += db.Name(cc) + " ; ";
				}
			std::sort(cons.self_negate.begin(), cons.self_negate.end());
			cons.self_negate.erase(std::unique(cons.self_negate.begin(),
											   cons.self_negate.end()),
								   cons.self_negate.end());
			std::printf("  discipline --no-self-negate : %zu effet(s) de "
						"negation derive(s) : %s\n",
						cons.self_negate.size(),
						listing.empty() ? "(aucun)" : listing.c_str());
		}
	}

	int major = 0, minor = 0;
	OCG_GetVersion(&major, &minor);
	std::printf("  ocgcore           : v%d.%d\n", major, minor);

	Arena arena;
	Arena* arena_ptr = nullptr;
	if(!opt.no_arena) {
		// Base fixe hors des plages usuelles : utile plus tard pour transporter
		// un instantane entre processus. Un echec est benin, Init retombe sur
		// une adresse libre.
		constexpr std::uintptr_t kPreferredBase = 0x0000400000000000ull;
		if(!arena.Init(opt.arena_mb << 20, kPreferredBase, error)) {
			std::printf("!! arene : %s\n", error.c_str());
			return 1;
		}
		arena_ptr = &arena;
		std::printf("  arene             : base 0x%llx, %zu Mo reserves, "
					"pages sales %s\n",
					static_cast<unsigned long long>(arena.BaseAddress()),
					opt.arena_mb,
					arena.DirtyTrackingAvailable() ? "suivies" : "INDISPONIBLES");
		if(std::string problem = arena.SelfCheck(); !problem.empty()) {
			std::printf("!! arene incoherente : %s\n", problem.c_str());
			return 1;
		}
	}

	int exit_code = 0;
	{
		// Le duel doit mourir avant l'arene : sa destruction libere dans l'arene.
		auto t_create = Clock::now();
		Duel duel(db, scripts, arena_ptr);
		if(!duel.Create(yrp->seed, yrp->duel_flags, yrp->start_lp, yrp->start_hand,
						yrp->draw_count, error)) {
			std::printf("!! %s\n", error.c_str());
			return 1;
		}
		double ms_create = MsSince(t_create);

		bool gc_stopped = false;
		if(opt.stop_gc && arena_ptr)
			gc_stopped = duel.SetLuaGc(false);

		auto t_setup = Clock::now();
		// --opp-hand en mode JUGE seulement : un replay produit avec une main
		// adverse augmentee ne se rejoue qu'avec la meme. En mode --solve, le
		// duel principal rejoue la REFERENCE, enregistree sans ces cartes —
		// les lui ajouter desynchroniserait le rejeu (les rips changent).
		const bool judge_opp_hand = !opt.solve && !cons.opp_hand.empty();
		if(judge_opp_hand)
			std::printf("  main adverse : +%zu carte(s) (--opp-hand)\n",
						cons.opp_hand.size());
		if(!duel.Setup(*yrp, error,
					   judge_opp_hand ? &cons.opp_hand : nullptr,
					   static_cast<uint8_t>(1 - opt.target_player))) {
			std::printf("!! %s\n", error.c_str());
			return 1;
		}
		double ms_setup = MsSince(t_setup);

		std::printf("\n=== rejeu de la ligne de reference ===\n");
		std::printf("  mode              : %s\n",
					yrp->IsHandTest() ? "HAND TEST" : "duel normal");
		std::printf("  ramasse-miettes   : %s\n",
					gc_stopped ? "ARRETE (memoire recuperee par restauration)"
							   : "actif");
		std::printf("  reponses a rejouer: %zu\n", yrp->responses.size());

		// Point de reprise pris AVANT la ligne : le test de fidelite consiste a
		// y revenir puis a rejouer les 290 decisions et a exiger un etat
		// rigoureusement identique.
		double ms_push = 0, ms_pop = 0;
		size_t push_bytes = 0;
		if(arena_ptr) {
			auto t = Clock::now();
			arena_ptr->Push();
			ms_push = MsSince(t);
			push_bytes = arena_ptr->LastPush().bytes;
		}

		LineResult first = RunLine(duel, *yrp, opt, true, &cons);
		ReportLine(first, *yrp, db, opt);

		// --- CHANTIER 0 : LE HARNAIS DE VALIDATION (session 19) --------------
		//
		// L'ordre est le livrable : extraire, IMPRIMER, puis confronter au plan
		// que le core vient de rejouer. Une table qui n'explique pas une ligne
		// dont on sait qu'elle est valide (0 MSG_RETRY) est une table fausse, et
		// tout ce qu'on batirait dessus le serait aussi.
		if(opt.operators) {
			ConstantTable kt;
			const size_t nconst = kt.Load(scripts);
			if(!nconst) {
				std::printf("\n!! --operators : aucune constante lue. Les "
							"fichiers `constant.lua` du jeu ne sont pas dans "
							"les --scriptdir : la table serait vide et le "
							"harnais rendrait un faux verdict.\n");
			} else {
				// Les codes du DECK, doublons compris : c'est le nombre de
				// COPIES qui decide d'une capacite « par COPIE », et la marche 1
				// en depend. Les codes hors decklist sont ajoutes ensuite, dans
				// `codes` seulement — les compter comme des copies fabriquerait
				// des capacites qui n'existent pas.
				std::vector<uint32_t> deck_codes;
				const size_t dk = static_cast<size_t>(opt.target_player);
				if(dk < yrp->decks.size()) {
					for(uint32_t c : yrp->decks[dk].main) deck_codes.push_back(c);
					for(uint32_t c : yrp->decks[dk].extra) deck_codes.push_back(c);
				}
				std::vector<uint32_t> codes = deck_codes;
				// Le board cible peut nommer des cartes hors decklist (mode
				// --target) : les lire aussi, sans quoi une activation de la
				// ligne tomberait en « carte hors table » pour une raison qui
				// n'a rien a voir avec l'extraction.
				for(const auto& [c, pos] : cons.board_add)
					codes.push_back(c);
				for(const ResolveReq& rq : cons.resolve_min)
					codes.push_back(rq.code);
				OperatorTable tbl;
				const size_t nread = tbl.Build(db, scripts, kt, codes);
				tbl.Print(db, kt);
				tbl.PrintGrants(db, kt);
				// La colonne NEGATIVE (chantier B). La zone du but est l'EXTRA
				// DECK : c'est la que dorment les copies que le but reclame, et
				// c'est la place dont 9.28 (h) a montre que douze aretes sur
				// treize la vidaient. Constante LUE, jamais ecrite en dur.
				uint64_t extra = 0;
				kt.Lookup("LOCATION_EXTRA", extra);
				tbl.PrintConsumption(db, kt, extra);
				// MARCHE 1. Trois `--target 54701958` ne sont pas trois buts :
				// c'est UN but a TROIS exemplaires, et c'est toute la
				// difference — la multiplicite est ce que `RecipeDistance` ne
				// porte pas et ce que le biais d'operateur ne sait pas designer.
				std::vector<std::pair<uint32_t, uint32_t>> goal_counts;
				for(const auto& [gc, gpos] : cons.board_add) {
					(void)gpos;
					auto git = std::find_if(
						goal_counts.begin(), goal_counts.end(),
						[&](const auto& g) { return g.first == gc; });
					if(git == goal_counts.end())
						goal_counts.emplace_back(gc, 1u);
					else
						++git->second;
				}
				tbl.PrintFiringCounts(db, kt, deck_codes, goal_counts);
				// Les demandes transitoires (s23) au banc aussi : le juge
				// gratuit doit voir exactement le modele que la recherche
				// verra — memes drapeaux, meme compilation.
				std::vector<std::pair<uint32_t, uint32_t>> goal_transient;
				if(!opt.resolve_legacy)
					for(const ResolveReq& rr : cons.resolve_min) {
						const uint32_t cc = db.Canonical(rr.code);
						if(std::find_if(goal_transient.begin(),
										goal_transient.end(),
										[&](const auto& g) {
											return g.first == cc;
										}) == goal_transient.end())
							goal_transient.emplace_back(cc, 1u);
					}
				// LE SOLVEUR SE PROUVE AVANT DE SERVIR (9.30). Cinq instances a
				// solution connue, couvrant les quatre theoremes. Un simplexe
				// faux rendrait des valeurs plausibles et NON admissibles :
				// c'est le seul point du chantier qui ne se demontre pas, donc
				// c'est le seul qui se teste a chaque execution.
				{
					size_t tot = 0;
					const size_t ok = SelfTestOperatorLP(&tot);
					std::printf("\n  auto-test du simplexe : %zu/%zu%s\n", ok,
								tot, ok == tot ? "" :
								"   <<< SOLVEUR FAUX : toute valeur h est a "
								"jeter");
					if(ok == tot)
						BuildAndSolveBalance(tbl, db, kt, deck_codes,
											 goal_counts, goal_transient);
				}
				// --- LE THEOREME 2, CONSTATE SUR UNE LIGNE REELLE -----------
				//
				// `h` est demontre consistant (9.30) : le long d'un plan, il ne
				// peut PAS descendre de plus que le cout du coup. Ce qui n'est
				// pas demontrable, c'est que le MODELE decrive ce jeu — et une
				// ligne valide de 331 decisions est le seul banc qui le dise.
				//
				// Trois lectures, et la troisieme est le but du chantier :
				//   - `h` doit finir a ZERO (le but est atteint) ;
				//   - aucune chute de plus de 1 par decision (sinon le modele
				//     ou le solveur ment, et la garde le dit sur place) ;
				//   - la DENSITE de descente, a comparer aux 15 % d'etats non
				//     muets de la nouveaute (9.29 (k)) : c'est le gradient que
				//     le score n'a jamais eu.
				if(!goal_counts.empty() && arena_ptr) {
					BalanceModel bm;
					if(bm.Build(tbl, db, kt, deck_codes, goal_counts,
								goal_transient)) {
						while(arena.Depth() > 1)
							arena.Pop();
						if(arena.Depth() == 0)
							arena.Push();
						arena.Restore();
						const uint8_t con =
							static_cast<uint8_t>(opt.target_player);
						std::vector<uint32_t> res, ava, fld, grv, rmv, fzn, tmp;
						auto snap = [&]() {
							res.clear(); ava.clear(); fld.clear();
							for(uint32_t loc : { 0x01u, 0x40u }) {   // DECK, EXTRA
								duel.QueryCodes(con, loc, tmp);
								res.insert(res.end(), tmp.begin(), tmp.end());
							}
							// DISPO = tout ce qui peut servir de materiau.
							for(uint32_t loc : { 0x02u, 0x04u, 0x08u, 0x10u,
												 0x20u }) {
								duel.QueryCodes(con, loc, tmp);
								ava.insert(ava.end(), tmp.begin(), tmp.end());
							}
							duel.QueryCodes(con, 0x04u, tmp);   // MZONE
							fld.assign(tmp.begin(), tmp.end());
							duel.QueryCodes(con, 0x10u, tmp);   // GRAVE (s21)
							grv.assign(tmp.begin(), tmp.end());
							duel.QueryCodes(con, 0x20u, tmp);   // BANNIE (s21)
							rmv.assign(tmp.begin(), tmp.end());
							// EN JEU (zone 6) = MZONE + SZONE.
							fzn = fld;
							duel.QueryCodes(con, 0x08u, tmp);
							fzn.insert(fzn.end(), tmp.begin(), tmp.end());
						};
						// --- PROFIL DES ECARTS ENTRE BARREAUX (s21) -----------
						// La forme close derivee en s21 sur 9.31 : le cout d'un
						// run serialise a blocs inegaux est Sigma b^(l_i),
						// DOMINE par b^(l_max). L'echelle de x* existe (16-18
						// cellules mesurees) et le run nu echoue quand meme : la
						// seule inconnue restante est le PROFIL des l_i le long
						// d'une ligne qui atteint le but. On le mesure ICI, sur
						// la meme marche que le banc du theoreme 2 — memes
						// besoins que la serialisation (NeedsFrom du bilan au
						// depart, RESERVE exclue), meme comptage que
						// SerialProgress.
						snap();
						LPResult r0;
						bm.Solve(res, ava, fld, &r0);
						std::vector<BalanceModel::Need> needs;
						if(r0.feasible) {
							for(const BalanceModel::Need& n : bm.NeedsFrom(r0))
								if(n.zone != 0)
									needs.push_back(n);
							// Les barreaux de CONSOMMATION (s21) — le meme
							// grain que la serialisation du chemin de
							// recherche, pour que ce banc juge l'echelle que
							// le run nu escalade vraiment.
							for(const BalanceModel::Need& n :
								bm.ConsumedFrom(r0))
								needs.push_back(n);
							// Les DEPARTS DE RESERVE (zone 5), memes deux
							// tranches que le cablage (`arch` = decalage).
							for(uint64_t off : { 0ull, 15ull }) {
								BalanceModel::Need dep;
								dep.zone = 5;
								dep.arch = off;
								dep.count = 15;
								needs.push_back(dep);
							}
							// Les HOTES DES EFFETS ACCORDES @TERRAIN, meme
							// derivation que le cablage (s21).
							{
								uint64_t k_add = 0, k_efm = 0;
								kt.Lookup("EFFECT_ADD_CODE", k_add);
								kt.Lookup("EFFECT_EXTRA_FUSION_MATERIAL",
										  k_efm);
								std::unordered_map<uint32_t, uint32_t> dcop;
								for(uint32_t c : deck_codes)
									++dcop[db.Canonical(c)];
								for(const auto& kvv : tbl.All()) {
									const uint32_t host =
										db.Canonical(kvv.first);
									auto dit = dcop.find(host);
									if(dit == dcop.end())
										continue;
									bool enabler = false;
									for(const DeclaredEffect& g :
										kvv.second.grants)
										enabler =
											enabler ||
											(g.code_is_effect &&
											 ((k_add &&
											   g.code_value == k_add) ||
											  (k_efm &&
											   g.code_value == k_efm)));
									if(!enabler)
										continue;
									BalanceModel::Need pres;
									pres.code = host;
									pres.zone = 6;   // EN JEU
									pres.count = (std::min)(dit->second, 3u);
									needs.push_back(pres);
								}
							}
						}
						const uint32_t res0b =
							static_cast<uint32_t>(res.size());
						auto served_now = [&]() {
							std::pair<uint64_t, uint32_t> out{ 0, 0 };
							uint32_t slot = 0;
							for(const BalanceModel::Need& n : needs) {
								uint32_t have = 0;
								if(n.zone == 5) {
									const uint32_t cur = static_cast<uint32_t>(
										res.size());
									const uint32_t dep =
										res0b > cur ? res0b - cur : 0;
									const uint32_t off =
										static_cast<uint32_t>(n.arch);
									have = dep > off ? dep - off : 0;
								} else {
								const std::vector<uint32_t>& pool =
									n.zone == 2   ? fld
									: n.zone == 3 ? grv
									: n.zone == 4 ? rmv
									: n.zone == 6 ? fzn
												  : ava;
								for(uint32_t raw : pool) {
									const uint32_t c = db.Canonical(raw);
									bool okc = false;
									if(n.code)
										okc = c == n.code;
									else if(const CardRow* row = db.Find(c))
										for(uint16_t sc : row->setcodes)
											if(sc && (sc & 0x0fffu) ==
														 (n.arch & 0x0fffu)) {
												okc = true;
												break;
											}
									if(okc && ++have >= n.count)
										break;
								}
								}
								const uint32_t got = (std::min)(have, n.count);
								out.second += got;
								if(slot < 16)
									out.first |=
										static_cast<uint64_t>(
											(std::min)(got, 15u))
										<< (slot * 4);
								++slot;
							}
							return out;
						};
						uint32_t max_served = served_now().second;
						// Attribution PAR SOUS-BUT (s21) : quelle exigence
						// chaque unite sert, et a quelle reponse. C'est ce qui
						// transforme « il y a un desert de 73 reponses » en
						// « le desert est ENTRE tel et tel sous-but » — la
						// question a laquelle un barreau de plus doit repondre.
						std::vector<uint32_t> got_max(needs.size(), 0);
						{
							uint32_t slot2 = 0;
							const uint64_t pk0 = served_now().first;
							for(size_t i = 0; i < needs.size() && slot2 < 16;
								++i, ++slot2)
								got_max[i] = static_cast<uint32_t>(
									(pk0 >> (slot2 * 4)) & 15u);
						}
						std::vector<size_t> unit_at;   // reponse du n-ieme +1
						std::vector<size_t> unit_need; // indice du sous-but servi
						std::vector<uint64_t> rung_vals;
						size_t at = 0, steps = 0, down = 0, up = 0, bad = 0;
						double prev = -1.0, first_h = -1.0, last_h = -1.0;
						size_t infeasible = 0;
						// LE DIAGNOSTIC (s22) : un « h = INFINI » au milieu d'une
						// ligne qui aboutit est une SUR-CONTRAINTE du modele, et
						// un compte seul ne dit pas laquelle. On garde les
						// premieres occurrences avec leurs lignes de phase 1.
						std::vector<std::pair<size_t, std::string>> inf_diag;
						// LA MARCHE AVEC QUOTAS (s24, chantier 4) : le JUGE
						// MANDATE du red-black. La meme marche, le meme LP, plus
						// les usages MSG_CHAINING accumules le long de la ligne
						// (meme derivation d'hotes et meme plafond de 12 que le
						// cablage du run). La ligne de reference ABOUTIT : tout
						// etat qu'elle traverse est vivant, donc tout « h_quota
						// = INFINI » sur un etat ou h etait fini est une
						// SUR-CONTRAINTE de l'agregat — 0 nouveau exige avant
						// de promouvoir le mecanisme au-dela du raffinement.
						std::vector<uint32_t> qwalk_hosts, qwalk_spent;
						std::vector<uint32_t> qwalk_chain;
						std::vector<uint32_t> qunb_union, qover_union;
						size_t infeasible_q = 0, infeasible_new = 0;
						double first_hq = -1.0, last_hq = -1.0;
						uint32_t qmax_applied = 0;
						std::vector<std::pair<size_t, std::string>> infq_diag;
						if(opt.quota_h && r0.feasible) {
							qwalk_hosts = bm.QuotaHostsFrom(r0, nullptr);
							if(qwalk_hosts.size() > 12)
								qwalk_hosts.resize(12);
							qwalk_spent.assign(qwalk_hosts.size(), 0u);
						}
						while(at < yrp->responses.size()) {
							qwalk_chain.clear();
							const size_t used = Advance(
								duel, *yrp, at, 1, nullptr,
								qwalk_hosts.empty() ? nullptr : &qwalk_chain);
							if(!used)
								break;
							at += used;
							++steps;
							for(uint32_t raw : qwalk_chain) {
								const uint32_t cq = db.Canonical(raw);
								for(size_t qi = 0; qi < qwalk_hosts.size(); ++qi)
									if(qwalk_hosts[qi] == cq)
										++qwalk_spent[qi];
							}
							snap();
							if(!needs.empty()) {
								const auto [pk, sv] = served_now();
								rung_vals.push_back(pk);
								for(size_t i = 0;
									i < needs.size() && i < 16; ++i) {
									const uint32_t g = static_cast<uint32_t>(
										(pk >> (i * 4)) & 15u);
									while(got_max[i] < g) {
										++got_max[i];
										unit_at.push_back(steps);
										unit_need.push_back(i);
									}
								}
								if(sv > max_served)
									max_served = sv;
							}
							LPResult lrx;
							const double h = bm.Solve(res, ava, fld, &lrx);
							// La marche jumelle avec quotas (s24) : memes
							// marquages, plus les capacites du chemin. Elle se
							// compare a `h` DECISION PAR DECISION — un
							// infaisable la ou h etait fini est un NOUVEAU, et
							// c'est lui que le juge compte.
							if(!qwalk_hosts.empty()) {
								std::vector<std::pair<uint32_t, uint32_t>> qsp;
								for(size_t qi = 0; qi < qwalk_hosts.size(); ++qi)
									if(qwalk_spent[qi])
										qsp.emplace_back(qwalk_hosts[qi],
														 qwalk_spent[qi]);
								LPResult lqx;
								const double hq =
									bm.Solve(res, ava, fld, &lqx, qsp);
								qmax_applied = (std::max)(qmax_applied,
														  lqx.quota_applied);
								auto once = [](std::vector<uint32_t>& v,
											   uint32_t c) {
									if(std::find(v.begin(), v.end(), c) ==
									   v.end())
										v.push_back(c);
								};
								for(uint32_t sk : lqx.quota_unbounded_hosts)
									once(qunb_union, sk);
								for(uint32_t sk : lqx.quota_overrun_hosts)
									once(qover_union, sk);
								if(hq < 0) {
									++infeasible_q;
									if(h >= 0) {
										++infeasible_new;
										if(infq_diag.size() < 6) {
											std::string rows;
											for(const std::string& lb :
												lqx.infeasible_rows) {
												if(!rows.empty())
													rows += " ; ";
												rows += lb;
											}
											if(rows.empty())
												rows = "(solveur en defaut)";
											infq_diag.emplace_back(steps, rows);
										}
									}
								} else {
									if(first_hq < 0)
										first_hq = hq;
									last_hq = hq;
								}
							}
							if(h < 0) {
								++infeasible;
								if(inf_diag.size() < 6) {
									std::string rows;
									for(const std::string& lb :
										lrx.infeasible_rows) {
										if(!rows.empty())
											rows += " ; ";
										rows += lb;
									}
									if(rows.empty())
										rows = "(solveur en defaut, pas une "
											   "preuve)";
									inf_diag.emplace_back(steps, rows);
								}
								continue;
							}
							if(first_h < 0)
								first_h = h;
							if(prev >= 0) {
								if(h < prev - 1.0 - 1e-6)
									++bad;      // chute > 1 : th. 2 VIOLE
								else if(h < prev - 1e-6)
									++down;
								else if(h > prev + 1e-6)
									++up;
							}
							prev = h;
							last_h = h;
						}
						arena.Restore();
						std::printf("\n=== h LE LONG DE LA LIGNE REELLE "
									"(theoreme 2) ===\n");
						std::printf("  %zu decision(s) parcourue(s) ; h : %.0f "
									"-> %.0f\n", steps, first_h, last_h);
						std::printf("  descentes %zu (%.0f %%), montees %zu, "
									"plats %zu\n", down,
									steps ? 100.0 * double(down) / double(steps)
										  : 0.0,
									up, steps - down - up - bad - infeasible);
						// CE COMPTEUR N'EST PAS UN JUGE DU THEOREME 2, et le dire
						// est le correctif. L'unite du theoreme est la
						// TRANSITION ; l'unite de cette marche est la DECISION,
						// et une seule decision resout parfois une chaine
						// entiere — donc plusieurs operateurs. Une chute de k
						// sur une decision est LEGITIME des que k operateurs ont
						// tire. Le premier tirage de ce banc l'a compte comme
						// une violation : c'etait l'instrument, pas le modele.
						std::printf("  chutes de plus de 1 sur UNE decision : "
									"%zu  (attendu : une decision resout parfois "
									"une chaine entiere)\n", bad);
						// TROIS CAS, ET LES CONFONDRE FERAIT MENTIR LE RAPPORT.
						// La premiere version imprimait « le modele RECONNAIT
						// le but » sur un but PROUVE IMPOSSIBLE (h = infini
						// partout, `last_h` reste a son sentinelle) : un
						// instrument qui felicite le modele quand il declare la
						// ligne morte est pire qu'aucun instrument.
						if(infeasible == steps && steps)
							std::printf("  h = INFINI sur TOUTE la ligne : le "
										"modele declare ce but hors d'atteinte "
										"(coherent avec l'impasse prouvee)\n");
						else if(last_h < 0)
							std::printf("  h final : INDETERMINE (aucun etat "
										"faisable rencontre)\n");
						else
							std::printf("  h final = %.0f  %s\n",
										std::fabs(last_h) < 1e-9 ? 0.0 : last_h,
										std::fabs(last_h) < 1e-6
											? "— le modele RECONNAIT le but"
											: "<<< le but est atteint et h ne "
											  "le voit pas : modele INCOMPLET");
						if(infeasible) {
							std::printf("  etats ou h = INFINI : %zu   <<< le "
										"modele declare morte une ligne qui "
										"ABOUTIT : sur-contrainte\n",
										infeasible);
							for(const auto& [st, rows] : inf_diag)
								std::printf("      decision %zu : %s\n", st,
											rows.c_str());
						}
						// --- le verdict de la marche avec quotas (s24) --------
						if(!qwalk_hosts.empty()) {
							std::printf("\n  --- la meme marche AVEC QUOTAS DU "
										"CHEMIN (--quota-h, s24) ---\n");
							std::printf("  hotes suivis : %zu ; lignes de "
										"quota posees (max sur la ligne) : "
										"%u\n",
										qwalk_hosts.size(), qmax_applied);
							// La raison de chaque hote ignore : `sans borne`
							// appelle l'extraction des bornes, `hors budget`
							// appelle la couverture du modele — les confondre
							// enverrait le correctif au mauvais endroit.
							auto qprint = [&db](const char* tag,
												const std::vector<uint32_t>& v) {
								if(v.empty())
									return;
								std::string sk;
								for(uint32_t c : v)
									sk += db.Name(c) + " ; ";
								std::printf("  ignores (%s) : %s\n", tag,
											sk.c_str());
							};
							qprint("effet SANS BORNE declaree — extraction",
								   qunb_union);
							qprint("observations > budget declare — "
								   "couverture", qover_union);
							std::printf("  h_quota : %.0f -> %.0f ; "
										"infaisables %zu (temoin sans quotas "
										"%zu)\n",
										first_hq, last_hq, infeasible_q,
										infeasible);
							std::printf("  etats infaisables NOUVEAUX : %zu  "
										"%s\n", infeasible_new,
										infeasible_new
											? "<<< SUR-CONTRAINTE red-black : "
											  "l'agregat ment sur cette ligne, "
											  "ne pas promouvoir"
											: "(JUGE : 0 nouveau — l'agregat "
											  "est sur le long de la "
											  "reference)");
							for(const auto& [st, rows] : infq_diag)
								std::printf("      decision %zu : %s\n", st,
											rows.c_str());
						}
						// --- le profil, et son verdict en forme close ---------
						if(!needs.empty() && !unit_at.empty()) {
							std::sort(rung_vals.begin(), rung_vals.end());
							rung_vals.erase(std::unique(rung_vals.begin(),
														rung_vals.end()),
											rung_vals.end());
							std::vector<size_t> gaps;
							size_t prev_at = 0;
							for(size_t p : unit_at) {
								gaps.push_back(p - prev_at);
								prev_at = p;
							}
							std::sort(gaps.rbegin(), gaps.rend());
							std::printf("\n=== PROFIL DES ECARTS ENTRE BARREAUX "
										"(s21, le long de la ligne reelle) ===\n");
							std::printf("  %zu unite(s) de sous-but servies en "
										"%zu reponses ; %zu palier(s) distincts "
										"de l'echelle\n", unit_at.size(), steps,
										rung_vals.size());
							// L'echelle NOMMEE : chaque barreau, sa reponse, et
							// l'ecart depuis le precedent. Les deserts se lisent
							// ici — entre QUELS sous-buts, pas seulement de
							// quelle longueur.
							{
								size_t pat = 0;
								for(size_t i = 0; i < unit_at.size(); ++i) {
									const BalanceModel::Need& n =
										needs[unit_need[i]];
									char nb[64];
									if(n.zone == 5)
										std::snprintf(nb, sizeof nb,
													  "(depart de reserve)");
									else if(n.code)
										std::snprintf(nb, sizeof nb, "%s",
													  db.Name(n.code).c_str());
									else
										std::snprintf(nb, sizeof nb,
													  "archetype 0x%llx",
													  (unsigned long long)n.arch);
									std::printf("    barreau %2zu  reponse %3zu "
												"(+%3zu)  %-34s @%s\n", i + 1,
												unit_at[i], unit_at[i] - pat, nb,
												n.zone == 2   ? "TERRAIN"
												: n.zone == 3 ? "CIMETIERE"
												: n.zone == 4 ? "BANNIE"
												: n.zone == 5 ? "RESERVE"
												: n.zone == 6 ? "EN JEU"
															  : "DISPO");
									pat = unit_at[i];
								}
							}
							std::printf("  ecarts entre unites consecutives, en "
										"REPONSES (adverses et forcees "
										"comprises),\n  tries decroissants :");
							for(size_t i = 0; i < gaps.size() && i < 12; ++i)
								std::printf(" %zu", gaps[i]);
							if(gaps.size() > 12)
								std::printf(" ...");
							std::printf("\n");
							// La forme close (s21) : cout ~ Sigma b^(l_i) en
							// decisions A CHOIX. Sur liger.yrpX, 170 des 331
							// reponses sont des decisions a choix (~0,51) ;
							// l'ecart en reponses MAJORE donc l'ecart utile.
							const double ratio = 0.51, b = 5.9;
							const double lmax =
								static_cast<double>(gaps.empty() ? 0 : gaps[0]);
							std::printf("  l_max = %.0f reponses (~%.0f "
										"decisions a choix) ; terme dominant "
										"b^l : 10^%.1f a b=%.1f\n", lmax,
										lmax * ratio,
										lmax * ratio * std::log10(b), b);
							std::printf("  Lecture : sous ~8 decisions a choix "
										"par ecart, chaque bloc est a portee "
										"de ~10^6 tirages ;\n  au-dela, c'est CE "
										"bloc qui interdit le run nu — l'option "
										"qui manque est ENTRE ces barreaux.\n");
						}
					}
				}
				std::printf("\n  (%zu script(s) lu(s), %zu introuvable(s))\n",
							nread, tbl.Missing());
				HarnessVerdict hv = ConfrontPlan(tbl, db, kt, first.activations);
				PrintVerdict(hv);
			}
		}

		// Verdict des contraintes sur CE replay, meme sans recherche : l'outil
		// sert aussi de JUGE — un replay produit hier (ou joue a la main) se
		// controle en le passant en entree avec les memes drapeaux.
		if(cons.Any()) {
			std::printf("\n--- verdict des contraintes sur ce replay ---\n");
			for(const auto& [n, allowed] : cons.summons) {
				if(n > first.summon_codes.size()) {
					std::printf("  --summon #%u : sans objet (%zu invocations)\n",
								n, first.summon_codes.size());
					continue;
				}
				uint32_t canon = db.Canonical(first.summon_codes[n - 1]);
				bool match = std::find(allowed.begin(), allowed.end(), canon) !=
							 allowed.end();
				std::printf("  --summon #%u : %s (%s)\n", n,
							match ? "respectee" : "VIOLEE",
							db.Name(canon).c_str());
			}
			if(!cons.guard.empty()) {
				std::printf("  --guard : %zu fenetre(s) adverse(s) sous menace, "
							"%zu decouverte(s)%s\n", first.guard_checks,
							first.guard_violations,
							first.guard_violations ? "  <-- VIOLEE" : "");
				if(first.guard_violations)
					std::printf("            premiere : apres l'invocation #%zu, "
								"main adverse %u carte(s)\n",
								first.first_guard_violation_summon,
								first.first_guard_violation_opp_hand);
			}
			if(!cons.no_activate.empty())
				std::printf("  --no-activate : %zu activation(s) interdite(s) "
							"utilisee(s)%s\n", first.forbidden_activations,
							first.forbidden_activations ? "  <-- VIOLEE" : "");
			for(size_t i = 0; i < cons.resolve_min.size(); ++i) {
				size_t have = i < first.resolve_counts.size()
					? first.resolve_counts[i] : 0;
				std::printf("  --resolve %s%s%s : %zu/%u%s\n",
							db.Name(cons.resolve_min[i].code).c_str(),
							cons.resolve_min[i].zones ? "@" : "",
							cons.resolve_min[i].zones
								? ZoneMaskName(cons.resolve_min[i].zones).c_str()
								: "",
							have, cons.resolve_min[i].min_count,
							have < cons.resolve_min[i].min_count
								? "  <-- VIOLEE" : "");
			}
		}

		std::printf("\n--- couts d'execution ---\n");
		std::printf("  chargement des bases       : %8.1f ms  (une fois par process)\n", ms_db);
		std::printf("  creation du duel + scripts : %8.1f ms  <-- paye a CHAQUE duel neuf\n", ms_create);
		std::printf("  mise en place des decks    : %8.1f ms\n", ms_setup);
		std::printf("  deroulement de la ligne    : %8.1f ms pour %zu decisions"
					"  (%.3f ms/decision)\n", first.ms, first.responses_used,
					first.responses_used ? first.ms / first.responses_used : 0.0);
		std::printf("  => re-simulation complete depuis la racine : %.1f ms\n",
					ms_create + ms_setup + first.ms);

		if(arena_ptr) {
			double ms_per_decision = first.responses_used
				? (first.ms - first.ms_write_watch) / first.responses_used : 0.0;
			ReportDirty(first, *arena_ptr, ms_per_decision);

			ArenaStats st = arena_ptr->Stats();
			std::printf("\n--- arene ---\n");
			std::printf("  engage            : %.2f Mo\n", st.committed / 1048576.0);
			std::printf("  zone servie       : %.2f Mo  (borne de l'instantane)\n",
						st.in_use / 1048576.0);
			std::printf("  blocs vivants     : %.2f Mo\n", st.live_bytes / 1048576.0);
			std::printf("  allocations       : %zu, liberations %zu\n",
						st.alloc_count, st.free_count);
			// Portee : CETTE arene (le thread principal). Le compteur etait un
			// `thread_local` lu ici, donc structurellement nul quoi qu'il
			// arrive : le mot « aucune » etait une propriete du code, pas une
			// mesure. Il est maintenant membre et atomique ; les arenes des
			// WORKERS se signalent, elles, par ReportPoison (C7).
			std::printf("  sorties d'arene   : %zu  %s\n", st.host_fallbacks,
						st.poisoned
							? "<-- ARENE CORROMPUE : etat hors instantane"
							: "(aucune sur l'arene principale ; les workers se "
							  "signalent separement)");

			std::printf("\n=== test de fidelite de la restauration ===\n");
			std::printf("  empilement initial        : %.2f Mo, %.2f ms\n",
						push_bytes / 1048576.0, ms_push);

			// Restore et non Pop : on garde le niveau pour pouvoir y revenir
			// autant de fois qu'on veut. C'est l'operation que le solveur
			// utilisera a chaque frere d'un noeud.
			auto t = Clock::now();
			arena_ptr->Restore();
			ms_pop = MsSince(t);
			std::printf("  restauration              : %.2f ms  (%.2f Mo recopies)\n",
						ms_pop, arena_ptr->LastRestore().bytes / 1048576.0);

			LineResult second = RunLine(duel, *yrp, opt, false);
			arena_ptr->Restore();   // le test de stress repart du debut de ligne

			bool ok = second.retries == 0 &&
					  second.responses_used == first.responses_used &&
					  second.turns == first.turns &&
					  second.fingerprint_final == first.fingerprint_final &&
					  second.fingerprint_at_target == first.fingerprint_at_target;
			std::printf("  rejeu apres restauration  : %zu reponses, %zu retries, "
						"%d tours\n", second.responses_used, second.retries,
						second.turns);
			std::printf("  empreinte board cible     : %016llx vs %016llx\n",
						(unsigned long long)first.fingerprint_at_target,
						(unsigned long long)second.fingerprint_at_target);
			std::printf("  empreinte etat final      : %016llx vs %016llx\n",
						(unsigned long long)first.fingerprint_final,
						(unsigned long long)second.fingerprint_final);
			std::printf("  => %s\n", ok
				? "IDENTIQUE : l'instantane capture bien tout l'etat du duel"
				: "DIVERGENCE : l'instantane laisse de l'etat dehors");
			if(!ok)
				exit_code = 1;

			if(ok)
				exit_code |= RunStressTest(duel, *yrp, opt, *arena_ptr,
										   first.fingerprint_final);
			if(ok && (opt.growth || opt.solve)) {
				// L'ordre compte : une recherche menee avec un enumerateur
				// incomplet ou un digest qui fusionne ne mesure rien.
				RunEnumeratorCheck(duel, *yrp, opt, *arena_ptr);
			}
			// La largeur se MESURE avant de promettre quoi que ce soit : c'est
			// elle qui calibre la patience de l'elagage par nouveaute.
			uint32_t patience = 0;
			if(ok && (opt.width || opt.solve))
				patience = MeasureWidth(duel, *yrp, opt, *arena_ptr, first);
			if(ok && opt.growth)
				RunGrowthMeasurement(duel, *yrp, opt, *arena_ptr, first);
			if(ok && !opt.fire_spec.empty()) {
				// Test adverse : mode exclusif — la menace est jouee pour de
				// vrai, la recherche refait le board depuis chaque injection.
				RunFireTest(duel, *arena_ptr, *yrp, opt, first, db, scripts,
							cons);
			} else if(ok && opt.solve) {
				// Une contrainte que la reference viole change la nature du
				// probleme meme-deck : la ligne enregistree N'EST PLUS une
				// solution, et la reparation — qui perturbe cette ligne — ne
				// mene nulle part. Le bon moteur est alors celui de la
				// transplantation (repertoire + NRPA), applique au meme deck :
				// meme board cible, ligne libre.
				bool ref_violates = first.guard_violations > 0 ||
									first.forbidden_activations > 0;
				for(size_t i = 0; i < cons.resolve_min.size(); ++i) {
					size_t have = i < first.resolve_counts.size()
						? first.resolve_counts[i] : 0;
					if(have < cons.resolve_min[i].min_count)
						ref_violates = true;
				}
				for(const auto& [n, allowed] : cons.summons) {
					if(n > first.summons_at_target)
						continue;
					uint32_t canon = db.Canonical(first.summon_codes[n - 1]);
					if(std::find(allowed.begin(), allowed.end(), canon) ==
					   allowed.end())
						ref_violates = true;
				}
				// Position de depart SYNTHETIQUE : decklist + main, sans
				// replay. La cible, l'adversaire et les parametres restent
				// ceux de la reference ; la main est forcee et VERIFIEE par
				// un duel jetable avant toute recherche.
				Replay synth;
				bool synth_ok = false;
				if(!opt.deck_file.empty()) {
					Deck ydk;
					std::string derr;
					std::vector<uint32_t> hand;
					bool ok_input = LoadYdk(opt.deck_file, ydk, derr);
					if(!ok_input)
						std::printf("!! %s\n", derr.c_str());
					if(ok_input) {
						if(!opt.hand_spec.empty()) {
							for(const std::string& item :
								SplitOn(opt.hand_spec, '|')) {
								if(item.empty())
									continue;
								uint32_t code = 0;
								if(!ResolveCard(item, db, "--hand", code)) {
									ok_input = false;
									break;
								}
								hand.push_back(code);
							}
						} else {
							// Main par defaut : celle de la reference.
							for(const auto& c : first.start_self.hand_cards)
								if(c.present)
									hand.push_back(db.Canonical(c.code));
						}
					}
					if(ok_input && hand.empty()) {
						std::printf("!! --deck : aucune main de depart\n");
						ok_input = false;
					}
					if(ok_input) {
						std::printf("\n=== depart synthetique ===\n");
						std::printf("  decklist : %s (%zu main, %zu extra)\n",
									opt.deck_file.c_str(), ydk.main.size(),
									ydk.extra.size());
						// Ce que le GABARIT apporte, et rien d'autre : sans ce
						// releve, « sans reference » n'est pas verifiable.
						std::printf("  gabarit  : %s\n"
									"             drapeaux 0x%llx, %u LP, main %u,"
									" pioche %u, adversaire %zu+%zu cartes\n",
									opt.replay.c_str(),
									(unsigned long long)yrp->duel_flags,
									yrp->start_lp, yrp->start_hand,
									yrp->draw_count,
									yrp->decks.size() > 1 ? yrp->decks[1].main.size() : 0,
									yrp->decks.size() > 1 ? yrp->decks[1].extra.size() : 0);
						std::printf("  main de depart :");
						for(uint32_t c : hand)
							std::printf(" %s;", db.Name(db.Canonical(c)).c_str());
						std::printf("\n");
						if(BuildSyntheticStart(*yrp, ydk, hand, db, scripts,
											   opt.arena_mb, synth, derr)) {
							std::printf("  main forcee et VERIFIEE sur un duel "
										"jetable.\n");
							synth_ok = true;
						} else {
							std::printf("!! %s\n", derr.c_str());
						}
					}
				}

				if(cons.AnyBoardEdit() && !synth_ok && !start_yrp) {
					std::printf("\n!! --board-add/--board-remove editent la "
								"CIBLE : exige --deck ou --start\n   (en "
								"reparation, la reference ne peut plus servir "
								"de controle sur une cible\n   qu'elle "
								"n'atteint pas).\n");
					exit_code = 1;
				} else if(synth_ok) {
					// LA BOUCLE INTERNE (s23, directive operateur ; forme
					// Go-Explore / Expert Iteration —
					// docs/etat-de-lart-boucle-interne.md §3). Une commande,
					// N rounds : chaque round est un run d'aujourd'hui a
					// l'octet pres (budget = part egale du restant) ; entre
					// deux rounds, la meilleure ligne JOINTE ecrite est
					// reinjectee comme approche du suivant — l'archive
					// Go-Explore qui persiste, sans que l'operateur n'itere.
					// Elle REMPLACE la ligne du round precedent (pas
					// d'empilement) ; les --approach de la commande restent.
					// rounds=1 : un appel, zero banniere — l'historique.
					Options ropt = opt;
					const double total_ms = opt.solve_ms;
					const auto tstart = Clock::now();
					const uint64_t nrounds = (std::max<uint64_t>)(1, opt.rounds);
					// Le transport inter-rounds (s24, --carry) : archive et
					// politique survivent aux appels — Go-Explore complet.
					RoundCarry rcarry;
					for(uint64_t round = 0; round < nrounds; ++round) {
						ropt.solve_ms = (std::max)(0.0,
							(total_ms - MsSince(tstart)) /
								double(nrounds - round));
						if(nrounds > 1) {
							char carried[64] = "";
							if(opt.carry && !rcarry.archive.empty())
								std::snprintf(carried, sizeof(carried),
											  ", archive portee %zu cellule(s)",
											  rcarry.archive.size());
							std::printf("\n===== ROUND %llu/%llu — budget "
										"%.0f s%s%s =====\n",
										(unsigned long long)(round + 1),
										(unsigned long long)nrounds,
										ropt.solve_ms / 1000.0,
										ropt.approach_files.size() >
												opt.approach_files.size()
											? ", ligne jointe reinjectee"
											: "", carried);
						}
						TransplantOutcome tout;
						RunTransplantSolve(duel, *yrp, synth, ropt, *arena_ptr,
										   first, db, scripts, patience, cons,
										   &tout, &rcarry);
						if(tout.solutions) {
							if(nrounds > 1)
								std::printf("\n===== ROUND %llu/%llu : %zu "
											"solution(s) — la boucle "
											"s'arrete =====\n",
											(unsigned long long)(round + 1),
											(unsigned long long)nrounds,
											tout.solutions);
							break;
						}
						if(round + 1 >= nrounds)
							break;
						// Sans ligne jointe NI archive portee, le round
						// suivant serait un simple re-run : on s'arrete. Sous
						// --carry, l'archive portee est une reinjection a
						// part entiere — la boucle continue sans ligne.
						if(tout.joint_file.empty() &&
						   !(opt.carry && !rcarry.archive.empty())) {
							std::printf("\n===== ROUND %llu/%llu : aucune "
										"ligne jointe ecrite — la boucle "
										"s'arrete =====\n",
										(unsigned long long)(round + 1),
										(unsigned long long)nrounds);
							break;
						}
						ropt.approach_files = opt.approach_files;
						if(!tout.joint_file.empty())
							ropt.approach_files.push_back(tout.joint_file);
					}
				} else if(!opt.deck_file.empty()) {
					// --deck demande mais depart inconstructible : ne pas
					// retomber en silence sur un autre mode.
					exit_code = 1;
				} else if(start_yrp)
					RunTransplantSolve(duel, *yrp, *start_yrp, opt, *arena_ptr,
									   first, db, scripts, patience, cons);
				else if(ref_violates) {
					// Une contrainte violee par la reference se corrige souvent
					// par une PETITE perturbation de sa ligne (reordonner deux
					// invocations) : la reparation a ecarts bornes est l'outil
					// exact de ce voisinage — les contraintes y forcent les
					// deviations au bon endroit. NRPA, qui reconstruit depuis
					// zero, ne vient qu'en secours, avec le reste du budget.
					std::printf("\n  La reference viole une contrainte de ligne."
								"\n  1) reparation a ecarts bornes SOUS "
								"contraintes (40%% du budget) ;\n  2) sinon, "
								"repertoire + NRPA sur le meme deck.\n");
					Options ropt = opt;
					ropt.solve_ms = opt.solve_ms * 0.4;
					size_t found = RunSolve(duel, *yrp, ropt, *arena_ptr, first,
											db, scripts, patience, cons);
					if(!found) {
						Options topt = opt;
						topt.solve_ms = opt.solve_ms * 0.6;
						RunTransplantSolve(duel, *yrp, *yrp, topt, *arena_ptr,
										   first, db, scripts, patience, cons);
					}
				} else {
					// --adapt n'est branche que sur les chemins --start/--fire
					// (BuildAdaptRuns). L'accepter ici sans le lire serait un
					// mecanisme silencieusement absent du chemin — la famille
					// exacte du piege « verifier qu'il a PU produire l'effet ».
					// C'est ainsi que la prevision des options (chantier 17) a
					// attendu deux sessions : son script tournait en mode
					// reparation, ou la table ne s'imprime jamais.
					if(!opt.adapt_files.empty())
						std::printf("\n!! --adapt est IGNORE en mode reparation "
									"(sans --start/--fire) : le corpus\n   "
									"n'entre que par le rejeu d'adaptation de la "
									"transplantation.\n");
					RunSolve(duel, *yrp, opt, *arena_ptr, first, db, scripts,
							 patience, cons);
				}
			}
		}

		if(first.retries)
			exit_code = 1;
		if(!scripts.Misses().empty()) {
			std::printf("\n  scripts introuvables (%zu) :", scripts.Misses().size());
			int shown = 0;
			for(const auto& m : scripts.Misses()) {
				if(shown++ >= 6) { std::printf(" ..."); break; }
				std::printf(" %s", m.c_str());
			}
			std::printf("\n");
		}
		// La vie du cache de scripts (s24) : combien de relectures disque le
		// run aurait payees sans lui. Un chiffre eleve nomme le trafic
		// « script charge apres le Push racine, annule par chaque Restore » —
		// invisible jusqu'ici parce que rien ne le comptait.
		if(scripts.CacheHits())
			std::printf("\n  cache de scripts : %llu relecture(s) disque "
						"evitee(s) (%zu script(s) en memoire)\n",
						(unsigned long long)scripts.CacheHits(),
						scripts.CacheEntries());
		if(g_depth_fallbacks.load(std::memory_order_relaxed))
			std::printf("\n  !! %llu recherche(s) de finisseur ont recu le "
						"plafond de profondeur de REPLI (%u decisions) :\n"
						"     leur prefixe atteignait deja le plafond global. "
						"Deux bras compares\n     « a budget egal » n'ont alors "
						"pas le meme budget de PROFONDEUR.\n",
						(unsigned long long)g_depth_fallbacks.load(
							std::memory_order_relaxed),
						kFinisherFallbackDepth);
		if(duel.EmptyProcessorStates())
			std::printf("\n  !! %zu requete(s) d'etat de processeur VIDES : le "
						"digest perd sa composante\n     de chaine, deux "
						"instants d'une meme resolution se confondent, et la "
						"branche\n     du combo est elaguee des le debut (patch "
						"C1 defait).\n",
						duel.EmptyProcessorStates());
		if(!scripts.Unreadable().empty()) {
			std::printf("\n  !! scripts PRESENTS mais illisibles (%zu) :",
						scripts.Unreadable().size());
			int shown = 0;
			for(const auto& u : scripts.Unreadable()) {
				if(shown++ >= 4) { std::printf(" ..."); break; }
				std::printf(" %s", u.c_str());
			}
			std::printf("\n     Le chargeur ne se rabat PAS sur un depot de rang "
						"inferieur (ce serait\n     une autre version du script) "
						"— ces cartes sont absentes de l'espace.\n");
			exit_code = 1;
		}
		if(!duel.Errors().empty()) {
			std::printf("\n  erreurs du core (%zu) :\n", duel.Errors().size());
			for(size_t i = 0; i < duel.Errors().size() && i < 6; ++i)
				std::printf("      %s\n", duel.Errors()[i].c_str());
		}
		// Codes absents de cards.cdb : le jumeau BASE DE DONNEES du decalage de
		// scripts, et plus silencieux que lui — le core donne a la carte
		// inconnue un corps vanille sans effet, le deck se charge, le duel
		// demarre, la ligne diverge, et rien ne le rapportait (4.6).
		if(!db.UnknownCodes().empty()) {
			std::printf("\n  !! codes absents de cards.cdb (%zu) : ",
						db.UnknownCodes().size());
			int shown = 0;
			for(uint32_t c : db.UnknownCodes()) {
				if(shown++ >= 8) { std::printf("..."); break; }
				std::printf("%u ", c);
			}
			std::printf("\n     Ces cartes sont des VANILLES SANS EFFET pour le "
						"core : toute ligne qui\n     les traverse est fausse. "
						"cards.cdb est perime ou incomplet.\n");
		}
		// Reponses que le decodeur de filtres n'a pas su lire (C10).
		if(UndecodableResponses()) {
			std::printf("\n  !! %llu reponse(s) INDECODABLES par le filtre "
						"--no-activate/--no-chain.\n     La disposition des "
						"messages du core a derive : les filtres ne veulent "
						"plus rien\n     dire, et ce run est a jeter.\n",
						(unsigned long long)UndecodableResponses());
			exit_code = 1;
		}
	}
	arena.Shutdown();
	// Cumul du run entier — les phases deja imprimees plus ce qui a tourne hors
	// d'elles (rejeu de reference, mesures du jalon 0...).
	prof::PrintTotal();
	return exit_code;
}
