#include "search.h"

#include <cstdio>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <queue>

namespace solver {
namespace {

uint64_t Mix(uint64_t h, uint64_t v) {
	h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
	return h;
}

// Melange un tampon par mots de 8 octets au lieu d'octet par octet (C22) : la
// longueur est melangee d'abord, la queue est completee de zeros — deux tampons
// distincts ne peuvent pas produire la meme sequence de mots. Les VALEURS de
// digest changent, pas leur pouvoir discriminant : le controle est l'egalite
// des faits structurels de la sante (273 digests deux a deux distincts).
uint64_t MixBytes(uint64_t h, const uint8_t* p, size_t n) {
	h = Mix(h, n);
	size_t i = 0;
	for(; i + 8 <= n; i += 8) {
		uint64_t v;
		std::memcpy(&v, p + i, 8);
		h = Mix(h, v);
	}
	if(i < n) {
		uint64_t tail = 0;
		std::memcpy(&tail, p + i, n - i);
		h = Mix(h, tail);
	}
	return h;
}

// Part MATERIELLE du score d'un tirage NRPA. Le score est material*1000 +
// nouveaute (cf. PolicyRollout) ; la nouveaute est consommee par la table au
// premier passage, donc rejouer la meme ligne rend un score legerement
// INFERIEUR, jamais egal. Comparer le score materiel est le critere de
// « re-trouvaille » des repetitions limitees (arXiv:2401.10420).
uint64_t MatScore(double score) {
	return score < 0 ? 0 : static_cast<uint64_t>(score / 1000.0);
}

constexpr uint32_t kBoardFlags = QUERY_CODE | QUERY_ALIAS | QUERY_POSITION |
								 QUERY_OVERLAY_CARD | QUERY_COUNTERS | QUERY_LINK;

// Zones CACHEES (main, cimetiere, banni, extra) : materiaux, compteurs et
// fleches de lien n'y existent pas — les cartes y ont perdu leurs overlays en
// quittant le terrain. Demander ces champs faisait serialiser (et parser) des
// octets vides a chaque requete du digest (C20). Les valeurs d'EntryOf sont
// INCHANGEES par construction : overlay et counters y etaient deja vides.
constexpr uint32_t kHiddenFlags = QUERY_CODE | QUERY_ALIAS | QUERY_POSITION;

// Une entree de board, independante de la colonne occupee. `goal_view` :
// equivalence de BUT — la position ATK/DEF est IGNOREE (arbitrage du joueur :
// deux boards qui ne different que par une position de combat sont le MEME
// board), seule la face (recto/verso) compte — un Junk Signal pose face verso
// n'est pas un Junk Signal face recto. Le digest d'ETAT, lui, garde la
// position complete : une position differente EST un etat de jeu different,
// et la fusionner ferait disparaitre des lignes sans le signaler.
// Entree RELACHEE : zone, code, face — et RIEN d'autre. C'est la comparaison
// qu'appelle une cible POSEE a la main (`--target`), qui est un code et une
// position et ne peut PAS porter de materiaux : un Xyz pose sans materiaux ne
// serait sinon egal a aucun Xyz reel, puisque tout Xyz sur le terrain en porte.
// Session 15 : c'etait la SECONDE raison, independante de la zone S/T vide,
// pour laquelle le but de l'etalon A etait insatisfiable — Bagooska figure dans
// toutes ses commandes depuis la session 7ter.
uint64_t LooseEntryOf(uint32_t loc_kind, uint32_t code, uint32_t position) {
	uint64_t h = Mix(loc_kind * 0x1000193ull, code);
	return Mix(h, (position & POS_FACEUP) ? POS_FACEUP : POS_FACEDOWN);
}

uint64_t EntryOf(uint32_t loc_kind, const QueriedCard& c, const CardDB& db,
				 bool goal_view = false) {
	uint64_t h = Mix(loc_kind * 0x1000193ull, c.Code());
	h = Mix(h, goal_view ? ((c.position & POS_FACEUP) ? POS_FACEUP
													  : POS_FACEDOWN)
						 : c.position);
	// Les materiaux comptent, leur ordre non.
	std::vector<uint32_t> ov;
	ov.reserve(c.overlay.size());
	for(uint32_t o : c.overlay)
		ov.push_back(db.Canonical(o));
	std::sort(ov.begin(), ov.end());
	for(uint32_t o : ov)
		h = Mix(h, o * 3ull);
	std::vector<uint32_t> ct = c.counters;
	std::sort(ct.begin(), ct.end());
	for(uint32_t k : ct)
		h = Mix(h, k * 5ull);
	return h;
}

} // namespace

void ComputeBoardKeyInto(Duel& duel, uint8_t con, BoardKey& key) {
	// Self : les deux Query internes s'imputent a leur propre sonde.
	prof::Scope ps(prof::kBoardKey);
	static thread_local std::vector<QueriedCard> cards;
	key.hash = 0;
	key.entries.clear();
	key.loose.clear();
	key.codes.clear();
	key.mzone_count = 0;
	for(uint32_t loc : { LOCATION_MZONE, LOCATION_SZONE }) {
		duel.Query(con, loc, kBoardFlags, cards);
		for(const auto& c : cards) {
			if(!c.present)
				continue;
			key.entries.push_back(EntryOf(loc, c, duel.Db(), /*goal_view=*/true));
			key.loose.push_back(LooseEntryOf(loc, c.Code(), c.position));
			key.codes.push_back(c.Code());
			if(loc == LOCATION_MZONE)
				++key.mzone_count;
		}
	}
	// Tri : deux boards identiques a permutation de colonnes pres doivent
	// donner la meme cle (criterium d'equivalence retenu, cf. section 1).
	std::sort(key.entries.begin(), key.entries.end());
	std::sort(key.loose.begin(), key.loose.end());
	std::sort(key.codes.begin(), key.codes.end());
	for(uint64_t e : key.entries)
		key.hash = Mix(key.hash, e);
}

BoardKey ComputeBoardKey(Duel& duel, uint8_t con) {
	BoardKey key;
	ComputeBoardKeyInto(duel, con, key);
	return key;
}

BoardKey MakeBoardKey(const std::vector<QueriedCard>& mzone,
					  const std::vector<QueriedCard>& szone, const CardDB& db) {
	BoardKey key;
	for(const auto& c : mzone) {
		if(!c.present)
			continue;
		key.entries.push_back(EntryOf(LOCATION_MZONE, c, db, /*goal_view=*/true));
		key.loose.push_back(
			LooseEntryOf(LOCATION_MZONE, db.Canonical(c.Code()), c.position));
		key.codes.push_back(c.Code());
		++key.mzone_count;
	}
	for(const auto& c : szone) {
		if(!c.present)
			continue;
		key.entries.push_back(EntryOf(LOCATION_SZONE, c, db, /*goal_view=*/true));
		key.loose.push_back(
			LooseEntryOf(LOCATION_SZONE, db.Canonical(c.Code()), c.position));
		key.codes.push_back(c.Code());
	}
	std::sort(key.entries.begin(), key.entries.end());
	std::sort(key.loose.begin(), key.loose.end());
	std::sort(key.codes.begin(), key.codes.end());
	for(uint64_t e : key.entries)
		key.hash = Mix(key.hash, e);
	return key;
}

uint32_t CommonCodes(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b) {
	uint32_t common = 0;
	size_t i = 0, j = 0;
	while(i < a.size() && j < b.size()) {
		if(a[i] == b[j]) { ++common; ++i; ++j; }
		else if(a[i] < b[j]) ++i;
		else ++j;
	}
	return common;
}

void CollectAtoms(Duel& duel, uint8_t con, const BoardKey& here,
				  uint32_t partition, std::vector<uint64_t>& out) {
	prof::Scope ps(prof::kAtoms);
	out.clear();
	const uint64_t p = partition * 0x9e3779b97f4a7c15ull;
	// Terrain : les entrees completes (code, position, materiaux, compteurs)
	// sont deja calculees — atomes fins. Le code seul est un atome a part :
	// "la carte est arrivee sur le terrain" est un fait en soi, meme si
	// position et materiaux changeront encore.
	for(uint64_t e : here.entries)
		out.push_back(Mix(p ^ 0x51, e));
	for(uint32_t c : here.codes)
		out.push_back(Mix(p ^ 0x52, c));
	// Zones cachees : (zone, carte, occurrence). C'est par le cimetiere et la
	// main que progresse la montee d'un combo — les cartes du board cible sont
	// des Synchro de fin de chaine, invisibles pendant 250 decisions.
	// L'occurrence distingue la deuxieme copie : envoyer un second exemplaire
	// au cimetiere est un fait neuf. QueryCodes : les codes suffisent, aucune
	// QueriedCard a construire (point chaud mesure, 4 requetes par decision).
	static thread_local std::vector<uint32_t> zone_codes;
	for(uint32_t loc : { LOCATION_HAND, LOCATION_GRAVE, LOCATION_REMOVED,
						 LOCATION_EXTRA }) {
		duel.QueryCodes(con, loc, zone_codes);
		std::sort(zone_codes.begin(), zone_codes.end());
		uint32_t prev = 0, occ = 0;
		for(uint32_t c : zone_codes) {
			occ = (c == prev) ? occ + 1 : 1;
			prev = c;
			out.push_back(Mix(Mix(p ^ (loc * 131ull), c), occ));
		}
	}
	out.push_back(Mix(p ^ 0x53, duel.Count(con, LOCATION_DECK)));
}

Search::Search(Duel& d, Arena& a, const Replay& y, const SearchConfig& c)
	: duel(d), arena(a), yrp(y), cfg(c) {
	stats.distinct_by_depth.assign(cfg.max_decisions + 2, 0);
	stats.expansions_by_depth.assign(cfg.max_decisions + 2, 0);
	// L'enumerateur compte ses propres troncatures dans NOS stats : un
	// sous-ensemble jamais emis est une branche absente de l'espace, au meme
	// titre qu'une coupure de plafond (C9).
	cfg.enumeration.subsets_capped = &stats.subsets_capped;
	// SONDE D'OFFRE (session 17) : l'enumerateur pose un bit par carte
	// surveillee presente dans le pool du prompt. `cfg` est une COPIE par
	// Search, donc pointer un membre est sur — chaque worker a la sienne.
	// Armee seulement sous --probe-repeat : hors sonde, aucun cout, pas meme
	// le test de pointeur dans la boucle de canonisation.
	if(cfg.probe_repeat && !cfg.probe_watch.empty()) {
		cfg.enumeration.watch = &cfg.probe_watch;
		cfg.enumeration.watch_offered = &offer_this_step;
	}
	for(const ResolveReq& req : cfg.resolve_min)
		resolve_total += req.min_count;
	// Graine de la borne brulees : les meilleures brulees des phases
	// precedentes bornent d'emblee (anytime seulement, marge comprise).
	if(cfg.anytime && cfg.burn_limit && cfg.burn_slack < 255)
		burn_cut = cfg.burn_limit + cfg.burn_slack;
}

uint32_t Search::CurrentBurned() {
	const auto con = static_cast<uint8_t>(cfg.target_player);
	return duel.Count(con, LOCATION_GRAVE) + duel.Count(con, LOCATION_REMOVED);
}

uint32_t Search::EffectiveBurnCut() const {
	uint32_t cut = burn_cut;
	if(cfg.shared_burn && cfg.anytime && cfg.burn_slack < 255) {
		const uint32_t g = cfg.shared_burn->load(std::memory_order_relaxed);
		if(g != UINT32_MAX && g + cfg.burn_slack < cut)
			cut = g + cfg.burn_slack;
	}
	return cut;
}

// Cout lexicographique empaquete : brulees, puis actions, puis decisions —
// comparable par un seul entier.
static inline uint64_t CostKey(uint32_t burned, uint32_t actions,
							   uint32_t decisions) {
	return (static_cast<uint64_t>(burned) << 44) |
		   (static_cast<uint64_t>((std::min)(actions, 0xFFFFFFu)) << 20) |
		   (std::min)(decisions, 0xFFFFFu);
}

bool Search::BudgetExhausted() const {
	// ARENE EMPOISONNEE : une allocation est sortie de l'arene, donc Restore()
	// ne reconstitue plus le duel. Tout ce qui suivrait porterait sur un etat
	// divergent — on arrete ici, et le worker le dit (C7). Une lecture atomique
	// relachee par noeud, sur un chemin qui fait deja une lecture d'horloge.
	if(arena.Poisoned()) {
		stats.arena_poisoned = true;
		return true;
	}
	if(stats.nodes >= cfg.max_nodes)
		return true;
	double ms = std::chrono::duration<double, std::milli>(
					std::chrono::steady_clock::now() - start).count();
	return ms >= cfg.time_limit_ms;
}

// Avance le duel jusqu'au prochain point de decision.
Search::Step Search::StepToPrompt() {
	actions_this_step = 0;
	turns_this_step = 0;
	summons_this_step.clear();
	resolved_this_step = 0;
	watch_this_step = 0;
	watch_act_this_step = 0;
	watch_zone_this_step = 0;
	material_violation = false;
	recent_materials.clear();
	recipe_materials.clear();
	saw_retry = false;
	for(;;) {
		int status = duel.Process();
		duel.Messages(msgs_scratch);
		for(const Message& m : msgs_scratch) {
			switch(m.type) {
			case MSG_MOVE:
				// Les materiaux d'une invocation portent REASON_MATERIAL : on
				// les accumule pour les attribuer a l'invocation qui suit dans
				// la meme resolution.
				//
				// La charge utile est code(4) | loc PRECEDENTE(10) | loc
				// courante(10) | raison(4) : l'octet 5 est donc la ZONE D'OU LE
				// MATERIAU A ETE PRIS. C'est elle qui fait du noeud du graphe de
				// recettes une EXIGENCE (« Leo Dancer au cimetiere ») et non une
				// carte — regle 1 du chantier 16.
				// SONDE DE PRESENCE EN ZONE (`--watch`, session 17). La zone de
				// DESTINATION est a l'offset 15 (loc_info courant : controleur
				// 14, location 15). Releve ici, donc a cout nul : MSG_MOVE est
				// deja decode, aucune requete de zone n'est ajoutee.
				//
				// C'est le seul volet de la sonde qui parle d'ETATS et non
				// d'evenements — et c'est celui dont l'etalon A a besoin :
				// `Lunalight Leo Dancer` n'est ni invoque ni active, son role
				// est d'ARRIVER AU CIMETIERE pour y etre banni comme materiau.
				if(!cfg.probe_watch.empty() && m.size >= 16) {
					uint32_t mc = 0;
					std::memcpy(&mc, m.data, 4);
					if(mc) {
						mc = duel.Db().Canonical(mc);
						const int slot = ZoneSlot(NormalizeZone(m.data[15]));
						if(slot >= 0)
							for(size_t i = 0;
								i < cfg.probe_watch.size() && i < 4; ++i)
								if(cfg.probe_watch[i] == mc)
									watch_zone_this_step |=
										1u << (8 * i + slot);
					}
				}
				if((!cfg.material_req.empty() || cfg.recipes) && m.size >= 28) {
					uint32_t code = 0, reason = 0;
					std::memcpy(&code, m.data, 4);
					std::memcpy(&reason, m.data + 24, 4);
					const uint8_t from = m.data[5];   // location precedente
					if(reason & REASON_MATERIAL) {
						// --material ne connait que le cas Synchro, mesure ;
						// le graphe de recettes, lui, prend TOUS les
						// mecanismes — c'est tout l'interet.
						if((reason & REASON_SYNCHRO) &&
						   !cfg.material_req.empty())
							recent_materials.push_back(code);
						if(cfg.recipes)
							recipe_materials.push_back(
								Requirement{ duel.Db().Canonical(code),
											 NormalizeZone(from) });
					}
				}
				break;
			case MSG_SUMMONING:
			case MSG_SPSUMMONING:
				// Le code de la carte invoquee est le premier u32 du message
				// (operations.cpp) — 0 si elle arrive face verso. C'est le
				// decompte qu'observent les contraintes d'invocation.
				if(m.size >= 4) {
					uint32_t code = 0;
					std::memcpy(&code, m.data, 4);
					summons_this_step.push_back(code);
					// SONDE PURE (`--watch`) : compter, et rien d'autre. Aucune
					// contrainte, aucun gradient, aucun biais d'indice — c'est
					// tout l'interet, cf. SearchConfig::probe_watch.
					if(!cfg.probe_watch.empty() && code) {
						const uint32_t wc = duel.Db().Canonical(code);
						for(size_t i = 0; i < cfg.probe_watch.size() && i < 4; ++i)
							if(cfg.probe_watch[i] == wc)
								watch_this_step += 1ull << (16 * i);
					}
					// Invocations surveillees (--summon-min) : meme compteur
					// packe que les resolutions.
					if(!cfg.resolve_min.empty() && code) {
						const uint32_t sc = duel.Db().Canonical(code);
						for(size_t i = 0; i < cfg.resolve_min.size(); ++i)
							if(cfg.resolve_min[i].on_summon &&
							   cfg.resolve_min[i].code == sc)
								resolved_this_step += 1ull << (16 * i);
					}
					// Contrainte de materiau : l'invocation surveillee doit
					// avoir consomme au moins une carte du bon attribut.
					if(!cfg.material_req.empty() && code) {
						uint32_t canon = duel.Db().Canonical(code);
						for(const auto& [card, attrs] : cfg.material_req) {
							if(card != canon)
								continue;
							bool ok = false;
							for(uint32_t mat : recent_materials) {
								const CardRow* row = duel.Db().Find(mat);
								if(row && (row->attribute & attrs)) {
									ok = true;
									break;
								}
							}
							if(!ok)
								material_violation = true;
						}
						// Chaque invocation consomme ses materiaux.
						recent_materials.clear();
					}
					// GRAPHE DE RECETTES : l'invocation qu'on vient de voir est
					// une recette OBSERVEE, avec ses materiaux et leurs zones
					// d'origine. Aucun texte de carte n'est lu (regle 3) — une
					// Fusion depuis la zone Pendule, un substitut de Fusion ou
					// une carte qui copie un nom s'enregistrent exactement de la
					// meme facon, en tant que fournisseurs de plus.
					if(cfg.recipes && code) {
						cfg.recipes->Observe(duel.Db().Canonical(code),
											 recipe_materials);
						++stats.recipes_seen;
					}
					recipe_materials.clear();
				}
				++actions_this_step;
				break;
			case MSG_FLIPSUMMONING:
				++actions_this_step;
				break;
			case MSG_CHAINING:
				// SONDE PURE, VOLET ACTIVATIONS (`--watch`, session 17).
				//
				// POURQUOI ELLE MANQUAIT, et ce que son absence a cache. La
				// sonde ne comptait que les INVOCATIONS, donc elle ne pouvait
				// rien dire des cartes dont le role est d'OUVRIR une voie. Or
				// c'est exactement le cas de l'etalon A : `Lunalight Leo Dancer`
				// exige un materiau nomme ABSENT DU DECK (Panther Dancer), il
				// n'est donc JAMAIS invocable par la voie normale — il ne peut
				// venir que de l'effet de `Lunalight Wolf` ou de
				// `Lunalight Masquerade`, qui invoquent une Fusion en
				// bannissant les materiaux depuis le terrain OU LE CIMETIERE.
				// « Leo n'est jamais invoque » ne disait donc pas si le solveur
				// avait seulement essaye la porte.
				//
				// Strictement observationnel, comme le volet invocations :
				// aucune contrainte, aucun gradient, aucun biais.
				if(!cfg.probe_watch.empty() && m.size >= 4) {
					uint32_t wcode = 0;
					std::memcpy(&wcode, m.data, 4);
					wcode = duel.Db().Canonical(wcode);
					for(size_t i = 0; i < cfg.probe_watch.size() && i < 4; ++i)
						if(cfg.probe_watch[i] == wcode)
							watch_act_this_step += 1ull << (16 * i);
				}
				// Resolutions surveillees (--resolve). Compte a l'activation :
				// en solitaire rien ne nie une chaine. Filtre par zone
				// d'ACTIVATION : l'Omega qui rippe s'active du terrain, son
				// effet de cimetiere ne compte pas (faux positif mesure).
				if(!cfg.resolve_min.empty() && m.size >= 4) {
					uint32_t code = 0;
					std::memcpy(&code, m.data, 4);
					code = duel.Db().Canonical(code);
					const uint32_t loc = ChainingLocation(m.data, m.size);
					for(size_t i = 0; i < cfg.resolve_min.size(); ++i)
						if(!cfg.resolve_min[i].on_summon &&
						   cfg.resolve_min[i].code == code &&
						   (!cfg.resolve_min[i].zones ||
							(loc & cfg.resolve_min[i].zones)))
							resolved_this_step += 1ull << (16 * i);
				}
				++actions_this_step;
				break;
			case MSG_NEW_TURN:
				++turns_this_step;
				break;
			case MSG_RETRY:
				saw_retry = true;
				break;
			default:
				break;
			}
			if(IsPrompt(m.type)) {
				prompt_type = m.type;
				prompt_payload.assign(m.data, m.data + m.size);
				prompt_player = m.size ? m.data[0] : -1;
			}
		}
		if(saw_retry)
			return Step::Rejected;   // reponse illegale : branche morte
		if(status == OCG_DUEL_STATUS_AWAITING)
			return Step::Prompt;
		if(status == OCG_DUEL_STATUS_END)
			return Step::Ended;
		if(status != OCG_DUEL_STATUS_CONTINUE)
			return Step::Rejected;
	}
}

uint64_t Search::Digest() const {
	return StateDigest(const_cast<Duel&>(duel), prompt_type, prompt_payload,
					   cfg.canonical_digest);
}

bool Search::FillChoices(ChoiceList& out) {
	// SONDE D'OFFRE : les bits valent pour LE prompt qu'on enumere maintenant.
	// Remis a zero ici et pas chez l'appelant, parce que FillChoices est le seul
	// point de passage de TOUTES les strategies — un oubli dans l'une d'elles
	// ferait fuiter une offre d'un prompt sur le suivant, en silence.
	offer_this_step = 0;
	EnumerateInto(prompt_type, prompt_payload.data(),
				  static_cast<uint32_t>(prompt_payload.size()),
				  cfg.enumeration, out);
	// L'adversaire ne joue pas : il passe systematiquement (perimetre solitaire
	// retenu). La derniere option est le "ne rien faire" des fenetres de chaine
	// et des questions oui/non.
	if(prompt_player != cfg.target_player)
		out.KeepOnlyLast();
	if(out.empty()) {
		// Prompt non enumerable : on tente la reponse par defaut plutot que de
		// laisser la branche mourir. C'est une REDUCTION A UNE BRANCHE, et elle
		// etait muette : un combo qui exige de declarer un nom de carte
		// (ANNOUNCE_*), de choisir un compteur ou de trier est structurellement
		// hors d'atteinte, et rien ne le signalait (3.5). Comptee par type de
		// prompt pour que le rapport dise LEQUEL.
		++stats.forced_default;
		stats.forced_default_prompts |=
			1ull << (prompt_type & 63);
		Choice& c = out.Emit();
		if(!DefaultResponse(prompt_type, prompt_payload.data(),
							static_cast<uint32_t>(prompt_payload.size()),
							c.response)) {
			out.Clear();
			return false;
		}
		if(cfg.enumeration.labels)
			c.label = "defaut";
	}
	return true;
}

// ATTRIBUTION DU DIGEST (session 17) — decoupe la cle de transposition en ses
// trois composantes, SANS changer sa valeur.
//
// LA QUESTION : aux points stables (prompt idle), le solveur maintient 284 etats
// distincts par BOARD, et le rapport CROIT avec la profondeur. La cle vaut
// zones + type + charge utile du prompt + etat du processeur ; les zones cachees
// sont deja canonicalisees par tri, donc le gonflement vient des deux derniers.
// LESQUELS, et dans quelle proportion ? Sans cette reponse, corriger serait
// parier — et la regle du dossier est l'attribution avant le correctif.
//
// `StateDigest` appelle cette fonction et combine EXACTEMENT dans le meme ordre :
// la valeur produite est identique a l'octet pres, donc aucune mesure anterieure
// n'est invalidee.
DigestParts StateDigestParts(Duel& d, uint8_t prompt_type,
							 const std::vector<uint8_t>& prompt_payload) {
	prof::Scope ps(prof::kDigest);
	static thread_local std::vector<QueriedCard> cards;
	static thread_local std::vector<uint64_t> entries;
	DigestParts out;
	uint64_t h = 0xcbf29ce484222325ull;
	// VARIANTE TRIEE du terrain, calculee en parallele et sans rien changer a
	// `h` : c'est elle qui ISOLE LE PRIX DE LA COLONNE. `StateDigest` conserve
	// l'ordre des zones de terrain (« la colonne pouvant compter : fleches de
	// lien, effets colonne-dependants ») — c'est un choix CONSERVATEUR, pas une
	// necessite, et `BoardKey` l'ignore explicitement de son cote. L'ecart entre
	// `zones` et `zones_sorted` dit exactement ce que ce choix coute.
	uint64_t hs = 0xcbf29ce484222325ull;
	static thread_local std::vector<uint64_t> field;
	for(uint8_t con = 0; con < 2; ++con) {
		field.clear();
		for(uint32_t loc : { LOCATION_MZONE, LOCATION_SZONE }) {
			h = Mix(h, loc * 131ull + con);
			d.Query(con, loc, kBoardFlags, cards);
			for(const auto& c : cards) {
				if(!c.present) { h = Mix(h, 1); continue; }
				const uint64_t e = EntryOf(loc, c, d.Db());
				h = Mix(h, e);
				// La zone entre dans l'entree triee, la COLONNE non : deux
				// monstres qui echangent leurs colonnes donnent la meme valeur.
				field.push_back(e ^ (loc * 0x9e3779b97f4a7c15ull));
			}
		}
		std::sort(field.begin(), field.end());
		hs = Mix(hs, con);
		for(uint64_t e : field)
			hs = Mix(hs, e);
		for(uint32_t loc : { LOCATION_HAND, LOCATION_GRAVE, LOCATION_REMOVED,
							 LOCATION_EXTRA }) {
			entries.clear();
			d.Query(con, loc, kHiddenFlags, cards);
			for(const auto& c : cards)
				if(c.present)
					entries.push_back(EntryOf(loc, c, d.Db()));
			std::sort(entries.begin(), entries.end());
			h = Mix(h, loc * 131ull + con);
			hs = Mix(hs, loc * 131ull + con);
			for(uint64_t e : entries) {
				h = Mix(h, e);
				hs = Mix(hs, e);
			}
		}
		h = Mix(h, d.Count(con, LOCATION_DECK));
		hs = Mix(hs, d.Count(con, LOCATION_DECK));
	}
	out.zones = h;
	out.zones_sorted = hs;
	h = Mix(h, prompt_type);
	h = MixBytes(h, prompt_payload.data(), prompt_payload.size());
	out.with_payload = h;
	const std::vector<uint8_t>& pstate = d.ProcessorState();
	out.procstate = MixBytes(0xcbf29ce484222325ull, pstate.data(), pstate.size());
	out.full = MixBytes(h, pstate.data(), pstate.size());
	return out;
}

uint64_t StateDigest(Duel& d, uint8_t prompt_type,
					 const std::vector<uint8_t>& prompt_payload,
					 bool sort_field) {
	// CANONICALISATION DES COLONNES (session 17). Le chemin trie passe par
	// StateDigestParts, qui calcule les deux variantes : c'est deux fois plus
	// cher, mais ce chemin n'est pris QUE sous le drapeau. Le chemin par defaut
	// ci-dessous est inchange a l'octet pres.
	if(sort_field) {
		const DigestParts p = StateDigestParts(d, prompt_type, prompt_payload);
		// Meme composition que la version ordonnee, en partant du terrain trie.
		uint64_t h = Mix(p.zones_sorted, prompt_type);
		h = MixBytes(h, prompt_payload.data(), prompt_payload.size());
		const std::vector<uint8_t>& ps2 = d.ProcessorState();
		return MixBytes(h, ps2.data(), ps2.size());
	}
	// Self : les 12 Query, les 2 Count et le ProcessorState internes vont a
	// leurs propres sondes ; ici ne reste que EntryOf + tri + melange.
	prof::Scope ps(prof::kDigest);
	static thread_local std::vector<QueriedCard> cards;
	static thread_local std::vector<uint64_t> entries;
	uint64_t h = 0xcbf29ce484222325ull;
	for(uint8_t con = 0; con < 2; ++con) {
		// Terrain : l'ordre des zones est conserve, la colonne pouvant compter
		// (fleches de lien, effets colonne-dependants).
		for(uint32_t loc : { LOCATION_MZONE, LOCATION_SZONE }) {
			h = Mix(h, loc * 131ull + con);
			d.Query(con, loc, kBoardFlags, cards);
			for(const auto& c : cards) {
				if(!c.present) { h = Mix(h, 1); continue; }
				h = Mix(h, EntryOf(loc, c, d.Db()));
			}
		}
		// Zones ou l'ordre n'a pas de sens de jeu : on canonicalise, sinon un
		// simple melange de main creerait un etat "different" et ferait
		// exploser la table de transposition pour rien.
		for(uint32_t loc : { LOCATION_HAND, LOCATION_GRAVE, LOCATION_REMOVED,
							 LOCATION_EXTRA }) {
			entries.clear();
			d.Query(con, loc, kHiddenFlags, cards);
			for(const auto& c : cards)
				if(c.present)
					entries.push_back(EntryOf(loc, c, d.Db()));
			std::sort(entries.begin(), entries.end());
			h = Mix(h, loc * 131ull + con);
			for(uint64_t e : entries)
				h = Mix(h, e);
		}
		h = Mix(h, d.Count(con, LOCATION_DECK));
	}
	h = Mix(h, prompt_type);
	h = MixBytes(h, prompt_payload.data(), prompt_payload.size());
	// Sans ceci, deux instants distincts d'une meme resolution de chaine —
	// meme terrain, meme main, meme prompt — sont confondus, et la branche du
	// combo est elaguee des le debut (patch C1).
	const std::vector<uint8_t>& pstate = d.ProcessorState();
	h = MixBytes(h, pstate.data(), pstate.size());
	return h;
}

void Search::Descend(uint32_t depth, uint32_t actions, uint32_t prompt_depth) {
	if(BudgetExhausted()) {
		stats.hit_time_limit = true;
		return;
	}
	// GARDE-FOU DE L'ELISION : un coup force n'avance pas `depth`, donc rien
	// n'arreterait une chaine de coups forces qui boucle. Le plafond porte sur
	// les PROMPTS et il est large — il ne doit mordre que sur une pathologie,
	// jamais sur une ligne normale (la reference en compte 284).
	if(prompt_depth > cfg.max_decisions * 8u + 64u) {
		++stats.edges_skipped;
		return;
	}
	Step st = StepToPrompt();
	uint32_t total_actions = actions + actions_this_step;

	if(st == Step::Rejected) {
		++stats.dead_ends;
		return;
	}
	if(st == Step::Ended) {
		++stats.terminals;
		return;
	}

	++stats.nodes;
	prof::Count(prof::kDecisions);
	if(depth < stats.expansions_by_depth.size())
		++stats.expansions_by_depth[depth];

	// Test de but : le board du joueur cible correspond-il ?
	if(cfg.collect_solutions && solutions.size() < cfg.max_solutions) {
		BoardKey here = ComputeBoardKey(duel, static_cast<uint8_t>(cfg.target_player));
		if(here == target) {
			Solution s;
			s.responses = path;
			s.actions = total_actions;
			s.decisions = depth;
			auto con = static_cast<uint8_t>(cfg.target_player);
			s.hand_left = duel.Count(con, LOCATION_HAND);
			s.deck_left = duel.Count(con, LOCATION_DECK);
			s.extra_left = duel.Count(con, LOCATION_EXTRA);
			s.burned = duel.Count(con, LOCATION_GRAVE) + duel.Count(con, LOCATION_REMOVED);
			solutions.push_back(std::move(s));
		}
	}

	// Coupures de PLAFOND : ce n'est pas l'espace qui s'arrete ici, c'est la
	// borne. Comptees, sans quoi "EPUISE" mentirait (C2). Testees AVANT
	// l'enumeration, comme a l'origine : enumerer un noeud qu'on va couper
	// serait un cout ajoute AU TEMOIN, et l'A/B ne mesurerait plus le mecanisme
	// mais le deplacement de cet appel.
	if(depth >= cfg.max_decisions) {
		++stats.edges_skipped;
		return;
	}
	if(cfg.max_actions && total_actions >= cfg.max_actions) {
		++stats.edges_skipped;
		return;
	}

	// --- ENUMERATION AVANT LA TABLE (session 17) ---------------------------
	//
	// L'ordre est inverse par rapport a l'origine, et c'est tout le mecanisme :
	// on ne peut pas savoir qu'un prompt est FORCE avant de l'avoir enumere, et
	// un prompt force ne doit couter ni entree de table, ni instantane, ni
	// profondeur. Sans `elide_forced`, le comportement est celui d'avant — la
	// seule difference est que `FillChoices` est appele avant la consultation de
	// la table plutot qu'apres, sur le meme etat et avec le meme resultat.
	ChoiceList& choices = ChoicesAt(prompt_depth);
	if(!FillChoices(choices)) {
		++stats.dead_ends;
		return;
	}
	// ATTRIBUTION GLOBALE : la repartition de TOUS les noeuds developpes. C'est
	// la mesure qui manquait quand la colonne a ete corrigee sur la foi d'un
	// comptage restreint aux points idle — une attribution sur un sous-ensemble
	// ne se transporte pas a l'ensemble.
	if(choices.size() <= 1)
		++stats.nodes_forced;
	else if(prompt_type == MSG_SELECT_IDLECMD)
		++stats.nodes_idle;
	else
		++stats.nodes_multi;

	// COUP FORCE : joue EN LIGNE. Pas de Push/Pop d'arene — il n'y a aucun frere
	// a restaurer, et l'instantane est un cout mesure (0,084 ms contre 0,056 ms
	// de travail utile, soit 149 % de surcout). Ni entree de table : un etat
	// sans alternative n'a rien a transposer. Ni profondeur : `depth` compte
	// desormais des DECISIONS, pas des prompts.
	if(cfg.elide_forced && choices.size() == 1) {
		++stats.elided;
		duel.SetResponse(choices[0].response);
		path.push_back(choices[0].response);
		Descend(depth, total_actions, prompt_depth + 1);
		path.pop_back();
		return;
	}

	// Transposition. Le budget restant est stocke avec l'etat : un etat resolu
	// avec peu de marge ne dispense pas de le reexplorer avec davantage.
	uint32_t remaining = cfg.max_decisions - depth;
	uint64_t key = Digest();
	auto it = tt.find(key);
	if(it != tt.end() && it->second >= remaining) {
		++stats.transpositions;
		return;
	}
	bool fresh = (it == tt.end());
	tt[key] = remaining;
	// BOARDS DISTINCTS (session 17). Compte APRES la transposition : on ne veut
	// pas mesurer combien de fois on repasse, mais combien de boards DIFFERENTS
	// les etats retenus realisent. Le rapport etats/boards est le prix paye pour
	// que la cle de transposition porte l'historique des effets (les compteurs
	// « une fois par tour » que l'API publique n'expose pas).
	if(cfg.count_boards) {
		ComputeBoardKeyInto(duel, static_cast<uint8_t>(cfg.target_player),
							board_scratch);
		seen_boards.insert(board_scratch.hash);
		uint64_t hl = 0, hc = 0;
		for(uint64_t e : board_scratch.loose)
			hl = hl * 0x100000001b3ull + e;
		for(uint32_t c : board_scratch.codes)
			hc = hc * 0x100000001b3ull + c;
		seen_loose.insert(hl);
		seen_codes.insert(hc);
		// POINTS STABLES : le prompt idle est le seul instant ou le board est
		// forme et ou deux etats de meme board devraient etre le meme noeud.
		if(prompt_type == MSG_SELECT_IDLECMD) {
			++stats.states_idle;
			seen_idle.insert(board_scratch.hash);
			stats.boards_idle = seen_idle.size();
			// ATTRIBUTION : les memes noeuds, comptes par composante de la cle.
			// C'est l'ecart entre ces quatre nombres qui designe le coupable —
			// corriger sans cette lecture serait un pari.
			const DigestParts dp =
				StateDigestParts(duel, prompt_type, prompt_payload);
			dz_set.insert(dp.zones);
			dzs_set.insert(dp.zones_sorted);
			dp_set.insert(dp.with_payload);
			dpr_set.insert(dp.procstate);
			df_set.insert(dp.full);
			stats.d_zones = dz_set.size();
			stats.d_zsort = dzs_set.size();
			stats.d_payload = dp_set.size();
			stats.d_proc = dpr_set.size();
			stats.d_full = df_set.size();
		}
		stats.boards_entries = seen_boards.size();
		stats.boards_loose = seen_loose.size();
		stats.boards_codes = seen_codes.size();
	}
	if(fresh && depth < stats.distinct_by_depth.size())
		++stats.distinct_by_depth[depth];

	arena.Push();
	for(const Choice& c : choices) {
		duel.SetResponse(c.response);
		path.push_back(c.response);
		Descend(depth + 1, total_actions, prompt_depth + 1);
		path.pop_back();
		arena.Restore();
		if(BudgetExhausted())
			break;
	}
	arena.Pop();
}

// DISTANCE SUR LE GRAPHE DE RECETTES (chantier 16).
//
// Somme, sur les cartes du board cible NON ENCORE POSEES, du nombre minimal
// d'invocations restant a faire d'apres les recettes observees. Les materiaux
// intermediaires comptent : c'est ce qui rend la distance DECROISSANTE en cours
// de ligne, la ou le `h` plat ne bouge pas tant qu'aucune carte cible n'est
// posee.
//
// LE TEST DE PRESENCE EST ZONE-AWARE, et c'est la regle 1 : « Leo Dancer au
// CIMETIERE » n'est pas satisfait par un Leo Dancer dans l'extra deck. Le
// releve coute UNE requete de zone par appel (cimetiere + banni), et n'est fait
// que dans RunLevin, au developpement d'un noeud — jamais dans les tirages.
//
// COUT PLANCHER, JAMAIS INFINI (regle 2) : un produit sans recette connue vaut
// 1. Le graphe vide rend donc exactement le `h` d'aujourd'hui, et l'activer ne
// peut pas rendre un but inatteignable.
// Combien d'entites presentes satisfont une exigence — et, au cadre RECLAMANT
// de DistanceLocked (la recette du haut et ses materiaux directs), quelles
// entites sont deja SERVIES : un meme corps ne peut plus etre a la fois le
// materiau nomme et l'un des « 3 monstres Lunalight » de la meme invocation
// (revue session 12). Recherche binaire sur la table triee pour une carte
// nommee, parcours pour une exigence cardinale (la table n'est triee ni sur
// l'archetype ni sur le niveau).
//
// AU NIVEAU DE L'ESPACE DE NOMS ET TEMPLATE (session 17). Deux appelants la
// partagent desormais : `RecipeDistance` (finisseur, table complete) et
// `RecipeEval` (tirages, table reduite aux zones exigees). Ils DOIVENT avoir
// exactement la meme semantique de reclamation, sans quoi la distance mesuree
// dans les tirages et celle du finisseur ne seraient pas la meme grandeur — et
// l'A/B comparerait deux definitions. Template parce que `Search::PresentInfo`
// est un type prive : l'instanciation se fait depuis les membres.
template<typename Info>
struct PresentAvailT {
	const std::vector<Requirement>& present;
	const std::vector<Info>& info;     // parallele a `present`
	std::vector<uint8_t>& claimed;     // parallele a `present`
	static bool ZoneOk(const Requirement& r, uint8_t z) {
		// Zone JOKER (exigence amorcee par le texte) : n'importe quelle zone ou
		// un materiau se PREND. Le deck et l'extra n'en sont pas : une carte qui
		// y dort doit encore etre invoquee, et c'est l'etape que l'on compte.
		return r.zone == kZoneAny ? ZoneIsPlayable(z) : z == r.zone;
	}
	bool CardinalMatch(const Requirement& r, size_t i) const {
		const Info& pi = info[i];
		if(!pi.row || !(pi.row->type & kRecipeTypeMonster))
			return false;
		if(!ZoneOk(r, pi.zone))
			return false;
		if(r.kind == kReqLevel)
			return (pi.row->level & 0xff) == r.code;
		for(uint16_t sc : pi.row->setcodes)
			if(sc && SetcodeMatches(sc, static_cast<uint16_t>(r.code)))
				return true;
		return false;
	}
	size_t NamedFirst(const Requirement& r) const {
		auto by_code = [](const Requirement& a, const Requirement& b) {
			if(a.code != b.code) return a.code < b.code;
			return a.zone < b.zone;
		};
		auto it = std::lower_bound(present.begin(), present.end(),
								   Requirement{ r.code, 0 }, by_code);
		return static_cast<size_t>(it - present.begin());
	}
	// Comptage PARTAGE (cadres non reclamants) : l'ancien comportement.
	uint32_t Count(const Requirement& r) const {
		if(r.kind != kReqCard) {
			uint32_t n = 0;
			for(size_t i = 0; i < info.size(); ++i)
				n += CardinalMatch(r, i) ? 1u : 0u;
			return n;
		}
		for(size_t i = NamedFirst(r);
			i < present.size() && present[i].code == r.code; ++i)
			if(ZoneOk(r, present[i].zone))
				return 1u;
		return 0u;
	}
	// Reclamation d'une copie nommee : la premiere qui convient et n'est pas
	// deja servie.
	bool Claim(const Requirement& r) {
		for(size_t i = NamedFirst(r);
			i < present.size() && present[i].code == r.code; ++i)
			if(!claimed[i] && ZoneOk(r, present[i].zone)) {
				claimed[i] = 1;
				return true;
			}
		return false;
	}
	// Cardinale reclamante : compte les entites non servies, en sert jusqu'a
	// `count`.
	uint32_t CountAndClaim(const Requirement& r) {
		uint32_t n = 0, taken = 0;
		for(size_t i = 0; i < info.size(); ++i) {
			if(claimed[i] || !CardinalMatch(r, i))
				continue;
			++n;
			if(taken < r.count) {
				claimed[i] = 1;
				++taken;
			}
		}
		return n;
	}
	void ResetClaims() { std::fill(claimed.begin(), claimed.end(), 0); }
};

float Search::RecipeDistance(const BoardKey& here, uint64_t resolved,
							 uint32_t probe_code, uint32_t* d_more) {
	if(!cfg.recipes) {
		if(d_more)
			*d_more = 0;
		return 0.0f;
	}
	prof::Scope ps(prof::kRecipe);
	const auto con = static_cast<uint8_t>(cfg.target_player);
	// Zones cachees, relevees une fois par appel. Le terrain vient de
	// `here.codes`, deja calcule par l'appelant : rien a repayer.
	recipe_present.clear();
	recipe_present_info.clear();
	// Terrain : deja calcule par l'appelant (`here.codes`), rien a repayer.
	for(uint32_t code : here.codes)
		recipe_present.push_back({ code, NormalizeZone(LOCATION_MZONE) });
	// Zones cachees : une requete chacune. C'est le cout du mecanisme, et il
	// n'est paye QUE dans RunLevin, au developpement d'un noeud — jamais dans
	// les tirages, ou il serait redhibitoire (audit 6.1).
	// Surcharge a TAMPON : la surcharge vecteur allouait un std::vector par
	// requete, cinq fois par noeud developpe (audit 6, gain le plus bete).
	static thread_local std::vector<QueriedCard> rq;
	for(uint32_t loc : { LOCATION_GRAVE, LOCATION_REMOVED, LOCATION_HAND,
						 LOCATION_EXTRA, LOCATION_DECK }) {
		duel.Query(con, loc, QUERY_CODE | QUERY_ALIAS, rq);
		for(const QueriedCard& c : rq)
			if(c.present)
				recipe_present.push_back(
					{ duel.Db().Canonical(c.Code()),
					  NormalizeZone(static_cast<uint8_t>(loc)) });
	}
	std::sort(recipe_present.begin(), recipe_present.end(),
			  [](const Requirement& a, const Requirement& b) {
				  if(a.code != b.code) return a.code < b.code;
				  return a.zone < b.zone;
			  });
	// Ce que chaque entite EST — niveau, archetypes, monstre ou non. Une seule
	// recherche de base par entite presente (~60-80 par noeud developpe), au
	// lieu d'une par exigence cardinale evaluee.
	if(cfg.recipes->HasCardinal()) {
		recipe_present_info.resize(recipe_present.size());
		for(size_t i = 0; i < recipe_present.size(); ++i)
			recipe_present_info[i] = { duel.Db().Find(recipe_present[i].code),
									   recipe_present[i].zone };
	}
	static thread_local std::vector<uint8_t> claim_scratch;
	claim_scratch.assign(recipe_present.size(), 0);
	// `info` doit etre parallele a `present` meme sans cardinaux : les
	// reclamations indexent les deux tables d'un meme indice.
	if(!cfg.recipes->HasCardinal())
		recipe_present_info.assign(recipe_present.size(), PresentInfo{});
	PresentAvailT<PresentInfo> avail{ recipe_present, recipe_present_info,
									  claim_scratch };

	// Cartes cibles manquantes : c'est sur elles que porte la distance.
	std::vector<uint32_t> missing;
	{
		std::vector<uint32_t> have = here.codes;
		for(uint32_t t : target.codes) {
			auto it = std::find(have.begin(), have.end(), t);
			if(it != have.end())
				have.erase(it);
			else
				missing.push_back(t);
		}
	}
	// UN seul appel : le verrou du graphe et la memoisation sont pris une fois
	// pour toutes les cartes manquantes.
	const uint32_t total = cfg.recipes->DistanceAll(
		missing, NormalizeZone(LOCATION_MZONE), avail);
	// SONDE DE REPETITION : « combien d'invocations pour un exemplaire DE PLUS ».
	// L'exemplaire FRAIS est retire du terrain avant de poser la question —
	// sinon `Claim` le trouverait present et rendrait 0, c'est-a-dire « tu en as
	// un », qui n'est pas ce qu'on demande. Le retrait se fait apres le calcul
	// ci-dessus pour que la distance au RESTE de la cible, elle, porte sur
	// l'etat reel. L'effacement preserve le tri, et les trois tables paralleles
	// (present / info / reclamations) perdent le meme indice.
	if(d_more) {
		*d_more = 0;
		if(probe_code) {
			const uint8_t onfield = NormalizeZone(LOCATION_MZONE);
			for(size_t i = 0; i < recipe_present.size(); ++i)
				if(recipe_present[i].code == probe_code &&
				   recipe_present[i].zone == onfield) {
					recipe_present.erase(recipe_present.begin() + i);
					if(i < recipe_present_info.size())
						recipe_present_info.erase(
							recipe_present_info.begin() + i);
					if(i < claim_scratch.size())
						claim_scratch.erase(claim_scratch.begin() + i);
					break;
				}
			claim_scratch.assign(recipe_present.size(), 0);
			static thread_local std::vector<uint32_t> one;
			one.assign(1, probe_code);
			*d_more = cfg.recipes->DistanceAll(one, onfield, avail);
		}
	}
	// Les resolutions exigees restent comptees comme avant : le graphe ne
	// modelise que les INVOCATIONS.
	(void)resolved;
	return static_cast<float>(total);
}

// INSTANTANE DU GRAPHE DE RECETTES (session 17) — la frontiere sure.
//
// Le graphe partage apprend PENDANT le run, donc il porte un mutex, donc il est
// interdit aux tirages : seize workers qui le prennent a chaque decision
// serialisent la recherche. On le copie donc chez soi tous les
// `cfg.recipe_snap_period` tirages, et l'on en derive d'un coup les trois objets
// que les chantiers 1, 3 et 4 consomment. Appelee A LA PROFONDEUR 0 d'un
// tirage, jamais en cours de ligne : `cfg.enumeration.assign_useful` pointe dans
// `snap_useful`, qu'un prompt en vol ne doit pas voir changer.
void Search::RecipeSnapshot() {
	if(!cfg.recipes)
		return;
	cfg.recipes->CopyInto(recipe_snap);
	snap_zone_mask = recipe_snap.ZoneMask();
	// Les racines sont les cartes de la CIBLE : la decomposition part du but,
	// c'est tout le principe de la recherche a rebours (Retro*, AO*).
	static thread_local std::vector<uint32_t> roots;
	roots = target.codes;
	std::sort(roots.begin(), roots.end());
	roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
	recipe_snap.Expand(roots, 3, snap_reqs, snap_backward);
	// CODES UTILES (chantier 1). Les exigences NOMMEES entrent telles quelles ;
	// les exigences CARDINALES (« 3 monstres Lunalight ») n'ont pas de code, on
	// les resout en balayant les cartes que le joueur POSSEDE — deck, extra,
	// main, cimetiere, terrain. Une carte absente de ces cinq zones ne peut de
	// toute facon pas servir de materiau. Ce balayage est le seul endroit du
	// mecanisme qui touche la liste des cartes, et il est paye une fois par
	// instantane, jamais par decision.
	snap_useful.clear();
	for(const Requirement& q : snap_reqs)
		if(q.kind == kReqCard)
			snap_useful.push_back(q.code);
	for(uint32_t c : roots)
		snap_useful.push_back(c);
	if(recipe_snap.HasCardinal()) {
		static thread_local std::vector<QueriedCard> rq;
		const auto con = static_cast<uint8_t>(cfg.target_player);
		for(uint32_t loc : { LOCATION_DECK, LOCATION_EXTRA, LOCATION_HAND,
							 LOCATION_GRAVE, LOCATION_MZONE }) {
			duel.Query(con, loc, QUERY_CODE | QUERY_ALIAS, rq);
			for(const QueriedCard& qc : rq) {
				if(!qc.present)
					continue;
				const uint32_t code = duel.Db().Canonical(qc.Code());
				const CardRow* row = duel.Db().Find(code);
				if(!row || !(row->type & kRecipeTypeMonster))
					continue;
				for(const Requirement& q : snap_reqs) {
					if(q.kind == kReqCard)
						continue;
					bool hit = false;
					if(q.kind == kReqLevel) {
						hit = (row->level & 0xff) == q.code;
					} else {
						for(uint16_t sc : row->setcodes)
							if(sc && SetcodeMatches(sc,
													static_cast<uint16_t>(q.code))) {
								hit = true;
								break;
							}
					}
					if(hit) {
						snap_useful.push_back(code);
						break;
					}
				}
			}
		}
	}
	std::sort(snap_useful.begin(), snap_useful.end());
	snap_useful.erase(std::unique(snap_useful.begin(), snap_useful.end()),
					  snap_useful.end());
	// Le branchement vers l'enumerateur ne se fait QUE sous --assign : les
	// chantiers 3 et 4 lisent le meme instantane sans toucher a l'espace
	// d'actions, et c'est ce qui les rend separables.
	cfg.enumeration.assign_useful =
		(cfg.assign && !snap_useful.empty()) ? &snap_useful : nullptr;
	++stats.recipe_snaps;
	stats.snap_products = recipe_snap.Products();
	stats.snap_useful = snap_useful.size();
	stats.snap_backward = snap_backward.size();
}

// DISTANCE DE RECETTES ET PROGRES A REBOURS, sur le chemin des TIRAGES.
//
// Meme grandeur que `RecipeDistance` (meme recursion, meme table de
// reclamation) mais payable a chaque decision, par trois economies :
//   - l'instantane LOCAL : le mutex n'est plus contendu ;
//   - les zones : on n'interroge que celles qu'une exigence MENTIONNE. Le
//     finisseur en releve cinq, DECK et EXTRA compris (~55 entites, la moitie
//     du cout) alors qu'aucune exigence amorcee ne les nomme (kZoneAny exclut
//     la reserve) ;
//   - le terrain sort GRATUITEMENT de `here`, deja calcule par l'appelant.
//
// UNE SEULE FONCTION POUR LES DEUX MESURES, parce que le cout EST le releve de
// presence : le refaire pour le progres a rebours doublerait le seul poste qui
// compte.
uint32_t Search::RecipeEval(const BoardKey& here, uint32_t* backward_out) {
	if(backward_out)
		*backward_out = 0;
	if(!recipe_snap.Products())
		return 0;
	prof::Scope ps(prof::kRecipe);
	const auto con = static_cast<uint8_t>(cfg.target_player);
	recipe_present.clear();
	recipe_present_info.clear();
	// Terrain : toujours, et sans masque — il ne coute rien (`here.codes` est
	// deja calcule) et c'est la zone dont le progres a rebours a besoin.
	for(uint32_t code : here.codes)
		recipe_present.push_back({ code, NormalizeZone(LOCATION_MZONE) });
	static thread_local std::vector<QueriedCard> rq;
	for(uint32_t loc : { LOCATION_GRAVE, LOCATION_REMOVED, LOCATION_HAND,
						 LOCATION_EXTRA, LOCATION_DECK }) {
		const uint8_t z = NormalizeZone(static_cast<uint8_t>(loc));
		// Les valeurs de NormalizeZone sont disjointes bit a bit : le masque se
		// teste par un simple ET.
		if(!(snap_zone_mask & z))
			continue;
		duel.Query(con, loc, QUERY_CODE | QUERY_ALIAS, rq);
		for(const QueriedCard& c : rq)
			if(c.present)
				recipe_present.push_back({ duel.Db().Canonical(c.Code()), z });
	}
	std::sort(recipe_present.begin(), recipe_present.end(),
			  [](const Requirement& a, const Requirement& b) {
				  if(a.code != b.code) return a.code < b.code;
				  return a.zone < b.zone;
			  });
	recipe_present_info.assign(recipe_present.size(), PresentInfo{});
	if(recipe_snap.HasCardinal())
		for(size_t i = 0; i < recipe_present.size(); ++i)
			recipe_present_info[i] = { duel.Db().Find(recipe_present[i].code),
									   recipe_present[i].zone };
	static thread_local std::vector<uint8_t> claim_scratch;
	claim_scratch.assign(recipe_present.size(), 0);
	PresentAvailT<PresentInfo> avail{ recipe_present, recipe_present_info,
									  claim_scratch };
	// PROGRES A REBOURS (chantier 4) : combien de sous-produits de la
	// decomposition sont deja DISPONIBLES. Calcule AVANT la distance, qui
	// consomme les reclamations.
	if(backward_out && !snap_backward.empty()) {
		uint32_t done = 0;
		for(uint32_t code : snap_backward) {
			auto it = std::lower_bound(
				recipe_present.begin(), recipe_present.end(),
				Requirement{ code, 0 },
				[](const Requirement& a, const Requirement& b) {
					if(a.code != b.code) return a.code < b.code;
					return a.zone < b.zone;
				});
			for(; it != recipe_present.end() && it->code == code; ++it)
				if(ZoneIsPlayable(it->zone)) {
					++done;
					break;
				}
		}
		*backward_out = done;
	}
	static thread_local std::vector<uint32_t> missing;
	missing.clear();
	{
		static thread_local std::vector<uint32_t> have;
		have = here.codes;
		for(uint32_t t : target.codes) {
			auto it = std::find(have.begin(), have.end(), t);
			if(it != have.end())
				have.erase(it);
			else
				missing.push_back(t);
		}
	}
	return recipe_snap.DistanceAll(missing, NormalizeZone(LOCATION_MZONE),
								   avail);
}

// HINDSIGHT (chantier 2) — les buts de substitution d'UN tirage.
//
// Andrychowicz et al., NeurIPS 2017 : un echec re-etiquete par le but qu'il a
// EFFECTIVEMENT atteint. Le score stocke n'est donc PAS celui du tirage (qui
// juge l'ancien but) mais le cout de CET accomplissement — au plus tot, au plus
// court. Sans ce re-etiquetage on garderait, pour « comment payer Perfume
// Dancer », la ligne qui a le plus de materiel au total : exactement le mauvais
// exemple.
void Search::HindsightCommit(
	const NrpaRun& run,
	const std::vector<std::pair<uint32_t, uint32_t>>& hits) {
	if(hits.empty() || run.steps.empty())
		return;
	for(const auto& [code, depth] : hits) {
		const double s = 1e12 - static_cast<double>(depth) * 1e5 -
						 static_cast<double>(run.steps.size());
		auto it = hindsight.find(code);
		if(it == hindsight.end()) {
			if(hindsight.size() >= cfg.hindsight_k)
				continue;
			HindsightGoal g;
			g.score = s;
			// SEULEMENT `steps` : c'est tout ce qu'AdaptRun consomme, et copier
			// `flat` (minage en ligne) doublerait la memoire du mecanisme pour
			// rien.
			g.run.steps = run.steps;
			hindsight.emplace(code, std::move(g));
			++stats.hindsight_goals;
		} else if(s > it->second.score) {
			it->second.score = s;
			it->second.run.steps = run.steps;
		}
	}
}

// SONDE D'OFFRE : ce prompt PROPOSAIT-IL la carte surveillee ?
//
// C'est la decomposition qui manquait a la session 16 — « jamais invoquee »
// recouvre « jamais proposee » (panne d'ETAT) et « proposee, jamais prise »
// (panne d'ECHANTILLONNAGE), et les deux appellent des chantiers opposes.
//
// EXTRAITE EN FONCTION parce qu'elle doit etre appelee A DEUX ENDROITS depuis
// que l'elision existe : un prompt force ne redescend pas dans le corps de la
// boucle, et compter plus bas perdait ses offres. Mesure de ce defaut avant
// correctif : Leo Dancer passait de 14 433 offres a 1 381, soit un denominateur
// divise par dix — donc un taux de conversion incomparable entre bras. Un
// instrument dont le denominateur bouge avec le drapeau qu'il juge ne juge rien.
void Search::CountOffers(uint64_t& rep_offered) {
	if(!cfg.probe_repeat || !offer_this_step)
		return;
	for(size_t i = 0; i < ProbeCount(); ++i) {
		if(!(offer_this_step & (1ull << i)))
			continue;
		++stats.rep[i].offer_steps;
		// Le TYPE de prompt, sans quoi « proposee » est indiscernable de
		// « listee par une revelation d'extra deck » (cf. offer_msgs).
		stats.rep[i].offer_msgs |= 1ull << (prompt_type & 63);
		if(const int slot = OfferSlot(prompt_type); slot >= 0)
			++stats.rep[i].offer_by[slot];
		if(!(rep_offered & (1ull << i))) {
			rep_offered |= 1ull << i;
			++stats.rep[i].offer_rollouts;
		}
	}
}

// LANDMARKS (chantier 18) : combien d'accomplissements restent a faire.
//
// LE POINT QUI REND LE MECANISME PAYABLE DANS LES TIRAGES. On ne releve pas
// l'etat, on releve les CLES DE LANDMARK — quelques dizaines de cases. Le
// terrain sort gratuitement de `here`, deja calcule par l'appelant ; une zone
// cachee n'est interrogee que si un landmark y vit (`ZoneMask`). Sur l'etalon A
// cela fait UNE requete (le cimetiere) la ou RecipeDistance en fait cinq, plus
// un tri et ~80 recherches de base — la raison pour laquelle celui-ci ne tourne
// que dans le finisseur et celui-la partout.
//
// REGLE 2 (heritee du graphe de recettes) : cette valeur ne coupe RIEN. Elle
// entre dans un score, jamais dans un test de vie ou de mort.
uint32_t Search::LandmarkRemaining(const BoardKey& here) {
	if(!cfg.landmarks || cfg.landmarks->Empty())
		return 0;
	const LandmarkGraph& g = *cfg.landmarks;
	lm_counts.assign(g.KeyCount(), 0);
	const auto con = static_cast<uint8_t>(cfg.target_player);
	const uint32_t mask = g.ZoneMask();
	constexpr uint8_t kOnField = 0x0c;
	if(mask & LandmarkGraph::ZoneBit(kOnField))
		for(uint32_t code : here.codes) {
			const size_t ix = g.IndexOf(LandmarkGraph::KeyOf(code, kOnField));
			if(ix != SIZE_MAX)
				++lm_counts[ix];
		}
	static thread_local std::vector<QueriedCard> rq;
	// Les deux cotes, et seulement les zones ou un landmark vit : le masque est
	// ce qui rend la facture proportionnelle a ce qui a ete APPRIS, et non au
	// nombre de zones qui existent.
	for(int side = 0; side < 2; ++side) {
		const uint8_t who = side ? (con ^ 1) : con;
		const uint8_t tag = side ? 0x80 : 0x00;
		for(uint32_t loc : { LOCATION_GRAVE, LOCATION_REMOVED, LOCATION_HAND }) {
			const uint8_t z = static_cast<uint8_t>(
				NormalizeZone(static_cast<uint8_t>(loc)) | tag);
			if(!(mask & LandmarkGraph::ZoneBit(z)))
				continue;
			duel.Query(who, loc, QUERY_CODE | QUERY_ALIAS, rq);
			for(const QueriedCard& c : rq)
				if(c.present) {
					const size_t ix = g.IndexOf(
						LandmarkGraph::KeyOf(duel.Db().Canonical(c.Code()), z));
					if(ix != SIZE_MAX)
						++lm_counts[ix];
				}
		}
	}
	const uint32_t rem = g.Remaining(lm_counts);
	stats.landmark_h_sum += rem;
	++stats.landmark_h_count;
	return rem;
}

// SONDE DE REPETITION (session 16) — le releve a la PREMIERE invocation du
// produit surveille. Un balayage de zones, une fois par tirage qui y arrive.
void Search::RepeatProbeFirst(size_t i, const BoardKey& here,
							  uint64_t resolved, uint32_t depth) {
	if(!cfg.recipes || i >= ProbeCount())
		return;
	RepeatProbe& rp = stats.rep[i];
	const uint32_t code = ProbeCode(i);
	rp.code = code;
	rp.known = cfg.recipes->Knows(code);
	uint32_t more = 0;
	const float rest = RecipeDistance(here, resolved, code, &more);
	++rp.more_n;
	rp.more_sum += more;
	rp.rest_sum += rest;
	rp.first_depth_sum += depth;
	rp.more_min = (std::min)(rp.more_min, more);
	rp.more_max = (std::max)(rp.more_max, more);
	// Le verdict tient dans cette comparaison : un deuxieme exemplaire aussi
	// proche que le premier l'etait au depart = le materiau a ete CONSERVE
	// (panne d'echantillonnage) ; plus loin = la chaine a ete CONSOMMEE (panne
	// de `h`). Sans reference mesuree, `more` seul ne dit rien.
	if(rp.d0 != 0xffffffffu) {
		if(more <= rp.d0)
			++rp.more_kept;
		else
			++rp.more_lost;
	}
}

uint32_t Search::Heuristic(const BoardKey& here) const {
	// Intersection sur les CODES, pas sur les entrees completes : une entree
	// complete inclut position et materiaux, qui n'arrivent qu'a la toute fin.
	// Elle vaudrait zero partout et ne guiderait rien. Le code, lui, donne un
	// gradient : chaque carte du board cible posee fait monter le score.
	uint32_t common = CommonCodes(here.codes, target.codes);
	// Bonus pour une correspondance exacte d'entree : departage deux etats qui
	// ont les memes cartes mais pas encore les bons materiaux.
	size_t a = 0, b = 0;
	uint32_t exact = 0;
	while(a < here.entries.size() && b < target.entries.size()) {
		if(here.entries[a] == target.entries[b]) { ++exact; ++a; ++b; }
		else if(here.entries[a] < target.entries[b]) ++a;
		else ++b;
	}

	// Les cartes du board cible sont des Synchro de fin de chaine : elles
	// n'arrivent qu'apres des centaines de decisions. Compter ces seules cartes
	// laisse l'heuristique a zero sur toute la montee et ne guide rien — c'est
	// ce qui faisait echouer la descente guidee.
	//
	// Ce qui progresse tout du long, c'est le MATERIEL : des corps sur le
	// terrain, et des monstres au cimetiere prets a etre recycles. On le compte
	// avec un poids faible devant les cartes cibles, pour orienter la montee
	// sans jamais primer sur l'objectif reel.
	//
	// Les corps sont deja comptes par ComputeBoardKey : c'etait le point chaud
	// (une requete de zone redondante par fils evalue).
	Duel& d = const_cast<Duel&>(duel);
	auto con = static_cast<uint8_t>(cfg.target_player);
	uint32_t fodder = d.Count(con, LOCATION_GRAVE);
	return common * 100 + exact * 10 + here.mzone_count * 3 + fodder;
}

uint32_t Search::ResolveProgress(uint64_t resolved) const {
	uint32_t progress = 0;
	for(size_t i = 0; i < cfg.resolve_min.size(); ++i) {
		uint32_t count = static_cast<uint32_t>((resolved >> (16 * i)) & 0xffff);
		progress += (std::min)(count, cfg.resolve_min[i].min_count);
	}
	return progress;
}

bool Search::GoalCheck(const BoardKey& here, uint32_t depth, uint32_t actions,
					   uint64_t resolved) {
	uint32_t common = CommonCodes(here.codes, target.codes);
	// Crete conditionnee aux resolutions completes : jusqu'ou montent les
	// lignes qui ont fait TOUS les rips ?
	if(resolve_total && ResolveProgress(resolved) >= resolve_total &&
	   common > stats.best_overlap_ripped)
		stats.best_overlap_ripped = common;
	if(common > stats.best_overlap) {
		stats.best_overlap = common;
		stats.best_board = here.codes;
		// Le chemin qui mene ici — copie rare (au plus 8 ameliorations par
		// recherche), c'est la matiere premiere du finisseur. Le detail du
		// terrain dit ce qui differe quand les codes y sont tous.
		stats.best_path = path;
		auto con = static_cast<uint8_t>(cfg.target_player);
		stats.best_mzone = duel.Query(con, LOCATION_MZONE, kBoardFlags);
		stats.best_szone = duel.Query(con, LOCATION_SZONE, kBoardFlags);
	}
	if(here.mzone_count > stats.best_monsters)
		stats.best_monsters = here.mzone_count;
	// But principal, ou un des buts ALTERNATIFS (test adverse --fire : le
	// board sans les cartes sacrifiees pour contrer la menace).
	// BUT PAR INCLUSION (--target-subset) ou par EGALITE EXACTE (defaut). Les
	// entrees sont triees des deux cotes, donc l'inclusion est un merge lineaire.
	auto reaches = [&](const BoardKey& want) {
		if(!cfg.goal_subset)
			return here == want;
		// Inclusion sur les entrees RELACHEES (zone, code, face) : une cible
		// posee ne porte ni materiaux ni compteurs, donc la comparer aux
		// entrees completes ne pourrait jamais reussir sur un Xyz.
		return std::includes(here.loose.begin(), here.loose.end(),
							 want.loose.begin(), want.loose.end());
	};
	bool alt_hit = false;
	if(!reaches(target)) {
		if(!cfg.target_alts)
			return false;
		for(const BoardKey& a : *cfg.target_alts)
			if(reaches(a)) {
				alt_hit = true;
				break;
			}
		if(!alt_hit)
			return false;
	}
	// Minimums de resolutions (--resolve) : un board conforme qui n'a pas
	// resolu ce qu'il faut n'est PAS une solution — et la ligne peut encore
	// l'accomplir plus loin, donc on n'elague pas : on continue.
	for(size_t i = 0; i < cfg.resolve_min.size(); ++i) {
		uint32_t count = static_cast<uint32_t>((resolved >> (16 * i)) & 0xffff);
		if(count < cfg.resolve_min[i].min_count)
			return false;
	}
	Solution s;
	s.responses = path;
	s.actions = actions;
	s.decisions = depth;
	s.alt = alt_hit;
	auto con = static_cast<uint8_t>(cfg.target_player);
	s.hand_left = duel.Count(con, LOCATION_HAND);
	s.deck_left = duel.Count(con, LOCATION_DECK);
	s.extra_left = duel.Count(con, LOCATION_EXTRA);
	s.burned = duel.Count(con, LOCATION_GRAVE) + duel.Count(con, LOCATION_REMOVED);
	++stats.goal_hits;
	// Cout du but atteint, lisible par l'appelant (score NRPA lexicographique)
	// meme quand l'enregistrement est refuse (doublon, pire que l'ensemble).
	goal_burned = s.burned;
	goal_actions = actions;
	goal_depth = depth;
	const uint64_t ck = CostKey(s.burned, s.actions, s.decisions);
	if(ck < best_cost_key)
		best_cost_key = ck;
	if(s.burned < best_burned_seen) {
		best_burned_seen = s.burned;
		// La borne B&B se resserre a chaque amelioration (anytime seulement).
		if(cfg.anytime && cfg.burn_slack < 255)
			burn_cut = best_burned_seen + cfg.burn_slack;
		// Publication aux autres workers (CAS min) : une amelioration ICI
		// coupe chez TOUS des la prochaine decision.
		if(cfg.shared_burn) {
			uint32_t cur = cfg.shared_burn->load(std::memory_order_relaxed);
			while(s.burned < cur &&
				  !cfg.shared_burn->compare_exchange_weak(
					  cur, s.burned, std::memory_order_relaxed)) {}
		}
	}
	if(!cfg.anytime) {
		solutions.push_back(std::move(s));
		return true;
	}
	// Anytime : dedup par chemin, puis remplacement du pire — l'ensemble est
	// borne, la recherche ne s'arrete pas.
	uint64_t h = 1469598103934665603ull;
	for(const auto& r : s.responses) {
		for(uint8_t b : r) {
			h ^= b;
			h *= 1099511628211ull;
		}
		h ^= 0xff;
		h *= 1099511628211ull;
	}
	if(!solution_hashes.insert(h).second)
		return true;
	if(solutions.size() < cfg.max_solutions) {
		solutions.push_back(std::move(s));
		return true;
	}
	size_t worst = 0;
	uint64_t worst_key = 0;
	for(size_t i = 0; i < solutions.size(); ++i) {
		const uint64_t k = CostKey(solutions[i].burned, solutions[i].actions,
								   solutions[i].decisions);
		if(k > worst_key) {
			worst_key = k;
			worst = i;
		}
	}
	if(ck < worst_key)
		solutions[worst] = std::move(s);
	return true;
}

bool GuardHolds(Duel& duel, uint8_t con, const std::vector<GuardClause>& clauses,
				const std::vector<uint32_t>& field_codes) {
	// Une requete par zone au plus, et seulement si une clause la mentionne.
	// Tampons reutilises entre les appels : la garde s'evalue a chaque fenetre
	// adverse, l'allocation par appel etait du gaspillage pur.
	struct ZoneCache {
		std::vector<uint32_t> codes;
		bool loaded = false;
	};
	static thread_local ZoneCache hand, grave, removed, extra;
	hand.loaded = grave.loaded = removed.loaded = extra.loaded = false;
	auto in_zone = [&](uint32_t loc, ZoneCache& z, uint32_t code) {
		if(!z.loaded) {
			duel.QueryCodes(con, loc, z.codes);
			for(uint32_t& c : z.codes)
				c = duel.Db().Canonical(c);
			std::sort(z.codes.begin(), z.codes.end());
			z.loaded = true;
		}
		return std::binary_search(z.codes.begin(), z.codes.end(), code);
	};
	auto atom_holds = [&](const GuardAtom& a) {
		if(a.zones & (LOCATION_MZONE | LOCATION_SZONE)) {
			if(std::binary_search(field_codes.begin(), field_codes.end(), a.code))
				return true;
		}
		if((a.zones & LOCATION_HAND) && in_zone(LOCATION_HAND, hand, a.code))
			return true;
		if((a.zones & LOCATION_GRAVE) && in_zone(LOCATION_GRAVE, grave, a.code))
			return true;
		if((a.zones & LOCATION_REMOVED) && in_zone(LOCATION_REMOVED, removed, a.code))
			return true;
		if((a.zones & LOCATION_EXTRA) && in_zone(LOCATION_EXTRA, extra, a.code))
			return true;
		return false;
	};
	for(const GuardClause& clause : clauses) {
		bool all = true;
		for(const GuardAtom& a : clause)
			if(!atom_holds(a)) { all = false; break; }
		if(all)
			return true;
	}
	return clauses.empty();
}

bool Search::GuardCut(const BoardKey& here, uint32_t summons) {
	if(cfg.guard_clauses.empty() || summons < cfg.guard_after)
		return false;
	// Seules comptent les fenetres ou l'ADVERSAIRE peut agir : c'est la que
	// Nibiru tomberait. Les etats intermediaires du joueur — meme depouilles —
	// sont hors de portee de l'adversaire.
	if(prompt_player == cfg.target_player || prompt_player < 0)
		return false;
	// Menace eteinte : la main adverse a ete suffisamment videe (handrip).
	if(cfg.guard_opp_hand_release >= 0 &&
	   static_cast<int>(duel.Count(static_cast<uint8_t>(1 - cfg.target_player),
								   LOCATION_HAND)) <= cfg.guard_opp_hand_release)
		return false;
	if(GuardHolds(duel, static_cast<uint8_t>(cfg.target_player),
				  cfg.guard_clauses, here.codes))
		return false;
	++stats.guard_cuts;
	return true;
}

bool Search::SummonsOk(uint32_t before) const {
	if(cfg.summon_constraints.empty() || summons_this_step.empty())
		return true;
	for(size_t i = 0; i < summons_this_step.size(); ++i) {
		auto it = cfg.summon_constraints.find(
			before + static_cast<uint32_t>(i) + 1);
		if(it == cfg.summon_constraints.end())
			continue;
		// Code canonique : la contrainte designe une carte, pas une
		// illustration. Un code 0 (invocation face verso) ne peut satisfaire
		// aucune contrainte — conservateur, et sans objet en pratique.
		uint32_t canon = duel.Db().Canonical(summons_this_step[i]);
		bool ok = false;
		for(uint32_t allowed : it->second)
			if(allowed == canon) { ok = true; break; }
		if(!ok)
			return false;
	}
	return true;
}

void Search::ArchiveObserve(const BoardKey& here, uint32_t depth,
							uint64_t resolved) {
	if(!cfg.archive_k)
		return;
	const uint32_t overlap = CommonCodes(here.codes, target.codes);
	const uint32_t rp = ResolveProgress(resolved);
	// Score : sous --resolve, les RESOLUTIONS d'abord — le verrou mesure est
	// la jonction rips+board, et les racines qui la franchissent sont les
	// etats deja rippes, pas les 8/8 muets. A egalite, le chemin le plus
	// COURT (plus de budget de profondeur pour le finisseur). En ANYTIME, le
	// cout partiel BAS s'insere avant le chemin court : les cellules utiles a
	// l'optimisation sont « board proche + brulees basses », pas seulement
	// « overlap haut » (chantier de re-parametrage, session 5).
	uint32_t burned = 0;
	auto pack = [&](uint32_t b) -> uint64_t {
		const uint64_t tail = 0xFFFFFFFFull - (std::min)(depth, 0xFFFFFF00u);
		if(!cfg.anytime)
			return cfg.resolve_min.empty()
				? (static_cast<uint64_t>(overlap) << 40) | tail
				: (static_cast<uint64_t>((std::min)(rp, 15u)) << 44) |
					  (static_cast<uint64_t>((std::min)(overlap, 255u)) << 36) |
					  tail;
		const uint64_t bb = 0xFFull - (std::min)(b, 0xFFu);
		return cfg.resolve_min.empty()
			? (static_cast<uint64_t>(overlap) << 40) | (bb << 32) | tail
			: (static_cast<uint64_t>((std::min)(rp, 15u)) << 48) |
				  (static_cast<uint64_t>((std::min)(overlap, 255u)) << 40) |
				  (bb << 32) | tail;
	};
	// QUOTA PAR NIVEAU DE PROGRES (session 15, --archive-spread). Le plancher
	// global est le mecanisme qui a rendu l'archive INUTILE en regime but seul
	// (9.21 (f)) : quand resolutions et overlap SATURENT — tout a r4 sur
	// l'etalon A — la cle de tri ne departage plus que par la longueur, et
	// l'archive se remplit d'etats de FIN DE LIGNE. Les 37 racines du finisseur
	// rendaient alors EPUISE en 0 a 13 expansions : elle rangeait ce qui est
	// TERMINAL, pas ce qui est prometteur, et le « premier retour puis explore »
	// de Go-Explore n'avait plus rien a explorer.
	// Le correctif est celui de Go-Explore lui-meme : une archive est une
	// COUVERTURE de cellules, pas un palmares. Chaque niveau de progres
	// (resolutions, cartes posees) recoit un quota, et un nouvel etat evince le
	// pire de SON niveau quand celui-ci est plein — jamais le meilleur d'un
	// niveau moins avance, qui est justement celui qui a encore de la ligne
	// devant lui.
	const uint32_t lvl = ArchiveLevel(rp, overlap);
	const bool spread = cfg.archive_spread;
	// Cas courant gratuit : le score OPTIMISTE (0 brulees) sous le plancher
	// evite les deux requetes de zone du compte reel. Sous quota, le plancher
	// qui s'applique est celui du NIVEAU de l'etat, et il ne s'applique que si
	// ce niveau est deja plein — sinon un etat peu avance serait refuse par le
	// palmares avant meme d'avoir sa place reservee.
	auto floor_of = [&](void) -> uint64_t {
		if(!spread)
			return archive.size() >= cfg.archive_k ? archive_min_score
												   : 0ull;
		auto ic = archive_levels.find(lvl);
		if(ic == archive_levels.end() || ic->second.count < ArchiveQuota(lvl))
			return 0ull;
		return ic->second.min_score;
	};
	uint64_t score = pack(0);
	if(uint64_t fl = floor_of(); fl && score <= fl)
		return;
	if(cfg.anytime) {
		burned = CurrentBurned();
		score = pack(burned);
		if(uint64_t fl = floor_of(); fl && score <= fl)
			return;
	}
	auto it = archive_cells.find(here.hash);
	if(it != archive_cells.end()) {
		ArchiveEntry& e = archive[it->second];
		if(score <= e.score)
			return;
		e.score = score;
		e.overlap = overlap;
		e.resolves = rp;
		e.decisions = depth;
		e.burned = burned;
		e.path = path;
	} else if(archive.size() < cfg.archive_k) {
		archive_cells.emplace(here.hash, archive.size());
		archive.push_back({ here.hash, score, overlap, rp, depth, burned, path });
	} else {
		size_t worst = 0;
		if(spread) {
			// La victime est le pire de CE niveau s'il est a quota ; sinon le
			// pire du niveau le PLUS PEUPLE — c'est ainsi qu'un niveau neuf se
			// fait de la place sans que le palmares le lui refuse.
			auto ic = archive_levels.find(lvl);
			const bool full_here =
				ic != archive_levels.end() && ic->second.count >= ArchiveQuota(lvl);
			uint32_t target_lvl = lvl;
			if(!full_here) {
				uint32_t best_count = 0;
				uint64_t best_min = ~0ull;
				for(const auto& [l, st] : archive_levels)
					if(st.count > best_count ||
					   (st.count == best_count && st.min_score < best_min)) {
						best_count = st.count;
						best_min = st.min_score;
						target_lvl = l;
					}
			}
			bool found = false;
			for(size_t i = 0; i < archive.size(); ++i) {
				if(ArchiveLevel(archive[i].resolves, archive[i].overlap) !=
				   target_lvl)
					continue;
				if(!found || archive[i].score < archive[worst].score) {
					worst = i;
					found = true;
				}
			}
			if(!found)
				return;
			// Un etat n'evince que dans SON niveau : ailleurs il prend une place
			// libre, sans avoir a battre l'occupant.
			if(target_lvl == lvl && score <= archive[worst].score)
				return;
		} else {
			for(size_t i = 1; i < archive.size(); ++i)
				if(archive[i].score < archive[worst].score)
					worst = i;
			if(score <= archive[worst].score)
				return;
		}
		archive_cells.erase(archive[worst].cell);
		archive_cells.emplace(here.hash, worst);
		archive[worst] = { here.hash, score, overlap, rp, depth, burned, path };
	}
	if(spread) {
		archive_levels.clear();
		for(const ArchiveEntry& e : archive) {
			LevelStat& st = archive_levels[ArchiveLevel(e.resolves, e.overlap)];
			++st.count;
			st.min_score = (std::min)(st.min_score, e.score);
		}
	}
	archive_min_score = ~0ull;
	for(const ArchiveEntry& e : archive)
		archive_min_score = (std::min)(archive_min_score, e.score);
	// Tant que l'archive n'est pas pleine, tout etat merite d'y entrer : le
	// plancher est donc nul, ce qui ANNULE l'early-out bon marche du haut de la
	// fonction pendant tout le remplissage. C'est voulu — sans cela on
	// refuserait des etats alors qu'il reste des cases libres — mais il faut le
	// lire ainsi : l'early-out ne travaille qu'a archive pleine (audit §5).
	if(archive.size() < cfg.archive_k)
		archive_min_score = 0;
}

bool Search::NoveltyCut(const BoardKey& here, uint32_t depth, uint64_t resolved,
						uint32_t& stale) {
	if(!cfg.novelty_patience)
		return false;
	// Partition sur les DEUX dimensions du but : cartes cibles posees ET
	// resolutions exigees faites — chaque rip rouvre la table, comme chaque
	// carte posee.
	uint32_t partition = cfg.novelty_serialize
		? CommonCodes(here.codes, target.codes) * 16u + ResolveProgress(resolved)
		: 0u;
	CollectAtoms(duel, static_cast<uint8_t>(cfg.target_player), here, partition,
				 atoms_scratch);
	if(novelty.Observe(atoms_scratch, depth, cfg.novelty_strict)) {
		++stats.novelty_novel;
		stale = 0;
		return false;
	}
	++stats.novelty_stale;
	if(++stale > cfg.novelty_patience) {
		++stats.novelty_cuts;
		return true;
	}
	return false;
}

// Renvoie true des qu'une solution est trouvee et qu'on a de quoi s'arreter.
bool Search::DescendGuided(uint32_t depth, uint32_t actions, uint32_t turns,
						   uint32_t summons, uint64_t resolved) {
	if(BudgetExhausted()) {
		stats.hit_time_limit = true;
		return false;
	}
	Step st = StepToPrompt();
	uint32_t total_actions = actions + actions_this_step;
	uint32_t total_turns = turns + turns_this_step;
	if(st == Step::Rejected) { ++stats.dead_ends; return false; }
	if(st == Step::Ended)    { ++stats.terminals; return false; }
	if(!SummonsOk(summons) || material_violation) {
		++stats.constraint_cuts;
		return false;
	}
	uint32_t total_summons =
		summons + static_cast<uint32_t>(summons_this_step.size());
	uint64_t total_resolved = resolved + resolved_this_step;

	++stats.nodes;
	prof::Count(prof::kDecisions);
	if(depth < stats.expansions_by_depth.size())
		++stats.expansions_by_depth[depth];

	ComputeBoardKeyInto(duel, static_cast<uint8_t>(cfg.target_player),
						board_scratch);
	const BoardKey& here = board_scratch;
	if(GoalCheck(here, depth, total_actions, total_resolved)) {
		if(!cfg.anytime)
			return solutions.size() >= cfg.max_solutions;
		// Anytime : NE PAS s'arreter au but — la descente continue sous les
		// bornes relachees. Les recuperations d'APRES-but (un effet de
		// cimetiere qui remelange des cartes au deck) reduisent les brulees
		// sans toucher au board : chaque re-atteinte re-enregistre si elle
		// est moins chere. C'est la classe de lignes que l'arret au but
		// rendait structurellement introuvable.
	}
	// Le board cible est celui de la fin du tour 1 : passe ce point, le board
	// du joueur est fige et toute la descente en dessous est du temps perdu.
	if(total_turns >= 2) {
		++stats.turn_cuts;
		return false;
	}
	if(GuardCut(here, total_summons))
		return false;
	ArchiveObserve(here, depth, total_resolved);

	if(depth >= cfg.max_decisions) {
		++stats.edges_skipped;
		return false;
	}
	if(cfg.max_actions && total_actions > cfg.max_actions) {
		++stats.edges_skipped;
		return false;
	}

	uint32_t remaining = cfg.max_decisions - depth;
	uint64_t key = Digest();
	auto it = tt.find(key);
	if(it != tt.end() && it->second >= remaining) {
		++stats.transpositions;
		return false;
	}
	if(it == tt.end() && depth < stats.distinct_by_depth.size())
		++stats.distinct_by_depth[depth];
	tt[key] = remaining;

	ChoiceList& choices = ChoicesAt(depth);
	if(!FillChoices(choices)) {
		++stats.dead_ends;
		return false;
	}

	// Evaluation des fils : on avance, on mesure, on restaure. C'est
	// exactement ce que l'instantane rend abordable (0,05 ms par retour).
	struct Scored { size_t index; uint32_t score; uint32_t acts; bool dead; };
	std::vector<Scored> scored;
	scored.reserve(choices.size());
	arena.Push();
	for(size_t i = 0; i < choices.size(); ++i) {
		duel.SetResponse(choices[i].response);
		Step cs = StepToPrompt();
		bool dead = (cs == Step::Rejected) || !SummonsOk(total_summons) ||
					material_violation;
		uint32_t sc = 0;
		if(!dead) {
			ComputeBoardKeyInto(duel, static_cast<uint8_t>(cfg.target_player),
								child_board_scratch);
			sc = Heuristic(child_board_scratch);
		}
		scored.push_back({ i, sc, actions_this_step, dead });
		arena.Restore();
	}
	// Le plus prometteur d'abord ; a score egal, celui qui coute le moins
	// d'actions (le critere de cout du solveur).
	std::stable_sort(scored.begin(), scored.end(),
					 [](const Scored& a, const Scored& b) {
						 if(a.score != b.score) return a.score > b.score;
						 return a.acts < b.acts;
					 });

	bool stop = false;
	for(const Scored& s : scored) {
		if(s.dead) { ++stats.dead_ends; continue; }
		duel.SetResponse(choices[s.index].response);
		path.push_back(choices[s.index].response);
		stop = DescendGuided(depth + 1, total_actions, total_turns,
							 total_summons, total_resolved);
		path.pop_back();
		arena.Restore();
		if(stop || BudgetExhausted())
			break;
	}
	arena.Pop();
	return stop;
}

bool Search::DescendRepair(uint32_t depth, uint32_t actions, size_t ref_index,
						   uint32_t disc, uint32_t stale, uint32_t turns,
						   uint32_t summons, uint64_t resolved) {
	if(BudgetExhausted()) {
		stats.hit_time_limit = true;
		return false;
	}
	Step st = StepToPrompt();
	uint32_t total_actions = actions + actions_this_step;
	uint32_t total_turns = turns + turns_this_step;
	if(st == Step::Rejected) { ++stats.dead_ends; return false; }
	if(st == Step::Ended)    { ++stats.terminals; return false; }
	if(!SummonsOk(summons) || material_violation) {
		++stats.constraint_cuts;
		return false;
	}
	uint32_t total_summons =
		summons + static_cast<uint32_t>(summons_this_step.size());
	uint64_t total_resolved = resolved + resolved_this_step;

	++stats.nodes;
	prof::Count(prof::kDecisions);
	if(depth < stats.expansions_by_depth.size())
		++stats.expansions_by_depth[depth];

	ComputeBoardKeyInto(duel, static_cast<uint8_t>(cfg.target_player),
						board_scratch);
	const BoardKey& here = board_scratch;
	if(GoalCheck(here, depth, total_actions, total_resolved)) {
		if(!cfg.anytime)
			return solutions.size() >= cfg.max_solutions;
		// Anytime : NE PAS s'arreter au but — la descente continue sous les
		// bornes relachees. Les recuperations d'APRES-but (un effet de
		// cimetiere qui remelange des cartes au deck) reduisent les brulees
		// sans toucher au board : chaque re-atteinte re-enregistre si elle
		// est moins chere. C'est la classe de lignes que l'arret au but
		// rendait structurellement introuvable.
	}
	if(total_turns >= 2) {
		++stats.turn_cuts;
		return false;
	}
	if(GuardCut(here, total_summons))
		return false;
	ArchiveObserve(here, depth, total_resolved);

	if(depth >= cfg.max_decisions) {
		++stats.edges_skipped;
		return false;
	}
	if(cfg.max_actions && total_actions > cfg.max_actions) {
		++stats.edges_skipped;
		return false;
	}

	// Elagage par nouveaute — jamais sur le prefixe pur (aucun ecart pris) :
	// c'est lui qui garantit qu'a zero ecart la reference est retrouvee, et
	// donc qu'"aucune solution" reste un signal de defaut.
	if(disc < cfg_discrepancies && NoveltyCut(here, depth, total_resolved, stale))
		return false;

	// Le budget d'ecarts est la contrainte qui lie : c'est lui qu'on memorise.
	uint64_t key = Digest();
	if(cfg.shared_tt) {
		// Table partagee entre workers (lazy SMP) : un etat resolu par l'un
		// elague chez tous.
		if(cfg.shared_tt->CheckAndClaim(key, disc + 1)) {
			++stats.transpositions;
			return false;
		}
	} else {
		auto it = tt.find(key);
		if(it != tt.end() && it->second >= disc + 1) {
			++stats.transpositions;
			return false;
		}
		if(it == tt.end() && depth < stats.distinct_by_depth.size())
			++stats.distinct_by_depth[depth];
		tt[key] = disc + 1;
	}

	// Resynchronisation SEMANTIQUE : si cet etat est un etat de la reference
	// PLUS LOIN dans sa ligne (meme digest — l'appariement d'etats de
	// LiftPlan), on y reprend la ligne : le suffixe enregistre redevient
	// lisible. Sans cela, toute deviation qui converge (echanger les
	// invocations #4/#5) laissait le reste des reponses illisible — mesure :
	// introuvable jusqu'a k=12.
	if(cfg.ref_digests && disc < cfg_discrepancies) {
		auto rit = cfg.ref_digests->find(key);
		if(rit != cfg.ref_digests->end() && rit->second > ref_index) {
			ref_index = rit->second;
			++stats.resyncs;
		}
	}

	// Candidats : la reponse enregistree d'abord (cout zero), puis les
	// alternatives (cout un ecart chacune).
	std::vector<std::pair<const std::vector<uint8_t>*, uint32_t>> cands;
	const std::vector<uint8_t>* recorded = nullptr;
	if(ref_index < yrp.responses.size()) {
		recorded = &yrp.responses[ref_index];
		// La reponse enregistree ne passe pas par l'enumerateur : le filtre
		// --no-activate doit la rattraper ici, sinon la ligne de reference
		// promenerait l'activation interdite en toute impunite.
		if(!ResponseForbidden(prompt_type, prompt_payload.data(),
							  static_cast<uint32_t>(prompt_payload.size()),
							  *recorded, cfg.enumeration))
			cands.emplace_back(recorded, 0u);
	}
	// Partition entre workers : tant qu'aucun ecart n'a ete pris, ce worker doit
	// reclamer le point avant d'y devier. Une fois le premier ecart consomme le
	// sous-arbre lui appartient, les ecarts suivants ne sont plus contraints.
	const uint32_t level = cfg_discrepancies - disc;   // 0 = aucun ecart pris
	bool mine = true;
	if(cfg.claims && level == cfg.claim_level)
		mine = cfg.claims->Claim(ref_index);

	// Repertoire FENETRE : apres une premiere deviation, un coup que la
	// reference joue a moins de `repair_window` decisions du point courant est
	// GRATUIT. C'est ce qui rend les permutations locales payables : echanger
	// les invocations #4/#5 (~17 decisions d'ecart, une activation entre les
	// deux) coutait une deviation PAR DECISION reordonnee — introuvable jusqu'a
	// k=12, mesure — et n'en coute plus qu'une, la resynchronisation par digest
	// raccrochant ensuite le suffixe exact. Jamais sur le prefixe pur.
	const bool windowed = disc < cfg_discrepancies && cfg.ref_keys &&
						  !cfg.ref_keys->empty();
	ChoiceList& alts = ChoicesAt(depth);
	alts.Clear();
	if((disc > 0 && mine) || windowed) {
		EnumerateInto(prompt_type, prompt_payload.data(),
					  static_cast<uint32_t>(prompt_payload.size()),
					  cfg.enumeration, alts);
		if(prompt_player != cfg.target_player && alts.size() > 1) {
			// L'adversaire ne joue pas : aucune deviation de son cote.
			alts.Clear();
		}
		size_t wlo = 0, whi = 0;
		if(windowed) {
			wlo = ref_index > cfg.repair_window ? ref_index - cfg.repair_window
												: 0;
			whi = (std::min)(ref_index + cfg.repair_window,
							 cfg.ref_keys->size());
		}
		auto free_move = [&](uint64_t pk) {
			if(!windowed || !pk)
				return false;
			for(size_t j = wlo; j < whi; ++j)
				if((*cfg.ref_keys)[j] == pk)
					return true;
			return false;
		};
		for(const Choice& c : alts) {
			if(recorded && c.response == *recorded)
				continue;
			uint32_t cost = free_move(c.plan_key) ? 0u : 1u;
			// Les deviations payantes restent soumises au budget et a la
			// partition entre workers ; les coups fenetres, non.
			if(cost == 1 && (disc == 0 || !mine)) {
				if(disc > 0)
					++stats.claim_denied;
				continue;
			}
			cands.emplace_back(&c.response, cost);
		}
	}
	if(cands.empty())
		return false;
	// Gratuits d'abord (la reponse enregistree reste en tete parmi eux).
	std::stable_sort(cands.begin(), cands.end(),
					 [](const std::pair<const std::vector<uint8_t>*, uint32_t>& a,
						const std::pair<const std::vector<uint8_t>*, uint32_t>& b) {
						 return a.second < b.second;
					 });

	arena.Push();
	bool stop = false;
	for(const auto& [resp, cost] : cands) {
		if(cost > disc)
			continue;
		duel.SetResponse(*resp);
		path.push_back(*resp);
		stop = DescendRepair(depth + 1, total_actions, ref_index + 1, disc - cost,
							 stale, total_turns, total_summons, total_resolved);
		path.pop_back();
		arena.Restore();
		if(stop || BudgetExhausted())
			break;
	}
	arena.Pop();
	return stop;
}

bool Search::DescendTransplant(uint32_t depth, uint32_t actions, uint32_t disc,
							   uint32_t stale, uint32_t turns,
							   uint32_t summons, uint64_t resolved) {
	if(BudgetExhausted()) {
		stats.hit_time_limit = true;
		return false;
	}
	Step st = StepToPrompt();
	uint32_t total_actions = actions + actions_this_step;
	uint32_t total_turns = turns + turns_this_step;
	if(st == Step::Rejected) { ++stats.dead_ends; return false; }
	if(st == Step::Ended)    { ++stats.terminals; return false; }
	if(!SummonsOk(summons) || material_violation) {
		++stats.constraint_cuts;
		return false;
	}
	uint32_t total_summons =
		summons + static_cast<uint32_t>(summons_this_step.size());
	uint64_t total_resolved = resolved + resolved_this_step;

	++stats.nodes;
	prof::Count(prof::kDecisions);
	if(depth < stats.expansions_by_depth.size())
		++stats.expansions_by_depth[depth];

	ComputeBoardKeyInto(duel, static_cast<uint8_t>(cfg.target_player),
						board_scratch);
	const BoardKey& here = board_scratch;
	if(GoalCheck(here, depth, total_actions, total_resolved)) {
		if(!cfg.anytime)
			return solutions.size() >= cfg.max_solutions;
		// Anytime : NE PAS s'arreter au but — la descente continue sous les
		// bornes relachees. Les recuperations d'APRES-but (un effet de
		// cimetiere qui remelange des cartes au deck) reduisent les brulees
		// sans toucher au board : chaque re-atteinte re-enregistre si elle
		// est moins chere. C'est la classe de lignes que l'arret au but
		// rendait structurellement introuvable.
	}
	if(total_turns >= 2) {
		++stats.turn_cuts;
		return false;
	}
	if(GuardCut(here, total_summons))
		return false;
	ArchiveObserve(here, depth, total_resolved);

	if(depth >= cfg.max_decisions) {
		++stats.edges_skipped;
		return false;
	}
	if(cfg.max_actions && total_actions > cfg.max_actions) {
		++stats.edges_skipped;
		return false;
	}

	// Elagage par nouveaute. C'est ici qu'il travaille le plus : la table de
	// transposition ne rattrapait que 15 % des etats a un ecart, parce qu'elle
	// ne fusionne que les etats IDENTIQUES. Le prefixe pur repertoire (aucun
	// ecart pris) reste exempt : le repertoire est le guide, pas l'explore.
	if(disc < cfg_discrepancies && NoveltyCut(here, depth, total_resolved, stale))
		return false;

	uint64_t key = Digest();
	if(cfg.shared_tt) {
		// Table partagee entre workers (lazy SMP) : un etat resolu par l'un
		// elague chez tous.
		if(cfg.shared_tt->CheckAndClaim(key, disc + 1)) {
			++stats.transpositions;
			return false;
		}
	} else {
		auto it = tt.find(key);
		if(it != tt.end() && it->second >= disc + 1) {
			++stats.transpositions;
			return false;
		}
		if(it == tt.end() && depth < stats.distinct_by_depth.size())
			++stats.distinct_by_depth[depth];
		tt[key] = disc + 1;
	}

	ChoiceList& choices = ChoicesAt(depth);
	if(!FillChoices(choices)) {
		++stats.dead_ends;
		return false;
	}

	// Visibilite des indices, cote recherche systematique.
	if(!cfg.hint_cards.empty()) {
		for(const Choice& c : choices) {
			if(c.card && std::find(cfg.hint_cards.begin(), cfg.hint_cards.end(),
								   c.card) != cfg.hint_cards.end()) {
				++stats.hint_seen;
				break;
			}
		}
	}

	// Repertoire : tout coup que la reference a joue est gratuit, ou qu'il soit
	// apparu dans sa ligne. Une reponse unique l'est aussi — il n'y a rien a
	// decider, et la facturer viderait le budget sur des non-choix.
	struct Cand { size_t index; uint32_t cost; size_t rank; };
	std::vector<Cand> cands;
	cands.reserve(choices.size());
	const bool forced = choices.size() == 1;
	size_t in_plan = 0;
	for(size_t i = 0; i < choices.size(); ++i) {
		auto it_plan = plan_index.find(choices[i].plan_key);
		if(it_plan != plan_index.end()) {
			cands.push_back({ i, 0u, it_plan->second });
			++in_plan;
		} else if(forced) {
			cands.push_back({ i, 0u, plan_index.size() });
		} else if(disc > 0) {
			cands.push_back({ i, 1u, plan_index.size() + i });
		}
	}

	if(cfg.trace) {
		std::printf("    d%-4u %-22s %2zu choix, %zu au repertoire%s\n", depth,
					PromptName(prompt_type), choices.size(), in_plan,
					(in_plan == 0 && !forced) ? "   <-- hors repertoire" : "");
		if(in_plan == 0 && !forced)
			for(const Choice& c : choices)
				std::printf("            propose : %s\n", c.label.c_str());
	}

	if(cands.empty())
		return false;
	// Gratuit d'abord, et parmi eux ceux que la reference jouait tot : c'est la
	// seule trace d'ordre que le repertoire conserve.
	std::stable_sort(cands.begin(), cands.end(),
					 [](const Cand& a, const Cand& b) {
						 if(a.cost != b.cost) return a.cost < b.cost;
						 return a.rank < b.rank;
					 });

	// Partition entre workers, au meme principe que RunRepair : on reclame au
	// N-ieme ecart, pas au premier — devier tot ouvre un sous-arbre enorme que
	// les autres workers doivent pouvoir partager.
	//
	// La cle est le DIGEST de l'etat : il n'y a pas d'indice lineaire ici, et en
	// mode but seul il n'y a meme pas de ligne. Deux etats distincts au niveau
	// de reclamation ouvrent deux sous-arbres disjoints — c'est la partition.
	const uint32_t level = cfg_discrepancies - disc;
	bool mine = true;
	if(cfg.claims && level == cfg.claim_level)
		mine = cfg.claims->Claim(key);

	arena.Push();
	bool stop = false;
	for(const Cand& c : cands) {
		if(c.cost > disc)
			continue;
		if(c.cost > 0 && !mine) {
			++stats.claim_denied;
			continue;   // sous-arbre pris par un autre worker
		}
		duel.SetResponse(choices[c.index].response);
		path.push_back(choices[c.index].response);
		stop = DescendTransplant(depth + 1, total_actions, disc - c.cost,
								 stale, total_turns, total_summons,
								 total_resolved);
		path.pop_back();
		arena.Restore();
		if(stop || BudgetExhausted())
			break;
	}
	arena.Pop();
	return stop;
}

bool Search::Rollout(uint64_t& rng) {
	auto next = [&rng] {
		rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
		return rng;
	};
	// Compteurs initiaux : un tirage peut demarrer au MILIEU d'une ligne
	// (finisseur : prefixe d'approche rejoue, puis echantillonnage).
	uint32_t actions = 0, turns = cfg.initial_turns, stale = 0,
			 summons = cfg.initial_summons;
	uint64_t resolved = cfg.initial_resolved;
	uint32_t rp_prev = ResolveProgress(resolved);
	bool hit = false;

	for(uint32_t depth = 0; depth < cfg.max_decisions; ++depth) {
		if(BudgetExhausted()) {
			stats.hit_time_limit = true;
			return hit;
		}
		Step st = StepToPrompt();
		actions += actions_this_step;
		turns += turns_this_step;
		if(st == Step::Rejected) { ++stats.dead_ends; return hit; }
		if(st == Step::Ended)    { ++stats.terminals; return hit; }
		if(!SummonsOk(summons) || material_violation) {
		++stats.constraint_cuts;
		return hit;
	}
		summons += static_cast<uint32_t>(summons_this_step.size());
		resolved += resolved_this_step;
		++stats.nodes;
		prof::Count(prof::kDecisions);
		if(resolved_this_step) {
			const uint32_t rp = ResolveProgress(resolved);
			for(uint32_t k = rp_prev; k < rp; ++k) {
				if(k < 4)
					++stats.resolve_reached[k];
				else
					++stats.resolve_overflow;   // histogramme tronque (4.10)
			}
			rp_prev = rp;
		}

		ComputeBoardKeyInto(duel, static_cast<uint8_t>(cfg.target_player),
							board_scratch);
		const BoardKey& here = board_scratch;
		if(GoalCheck(here, depth, actions, resolved)) {
			// Anytime : continuer — les recuperations d'apres-but peuvent
			// reduire les brulees, chaque re-atteinte re-enregistre.
			if(!cfg.anytime)
				return true;
			hit = true;
		}
		// Passe la fin du tour 1, le board est fige : continuer le tirage dans
		// le tour de l'adversaire ne peut plus rien atteindre.
		if(turns >= 2) {
			++stats.turn_cuts;
			return hit;
		}
		if(GuardCut(here, summons))
			return hit;
		if(const uint32_t bcut = EffectiveBurnCut();
		   bcut != UINT32_MAX && CurrentBurned() > bcut) {
			++stats.burn_cuts;
			return hit;
		}
		ArchiveObserve(here, depth, resolved);
		// Rollout-IW sans arbre : un tirage qui cesse de produire du neuf est
		// coupe. DESACTIVE PAR DEFAUT, sur mesure : la table etant partagee
		// entre tirages, re-parcourir le meme debut tue le tirage avant qu'il
		// ait pu devier (2/8 au lieu de 6/8 sur le cas de transplantation).
		if(cfg.novelty_rollout_cut && NoveltyCut(here, depth, resolved, stale))
			return hit;

		ChoiceList& choices = ro_choices;
		if(!FillChoices(choices)) {
			++stats.dead_ends;
			return hit;
		}

		size_t pick = 0;
		if(choices.size() > 1) {
			// On avance, on mesure, on restaure : c'est ce que l'instantane rend
			// abordable, et c'est ici que se joue toute la qualite du tirage.
			struct Scored { size_t index; int64_t score; bool known; bool dead; };
			std::vector<Scored> scored;
			scored.reserve(choices.size());
			arena.Push();
			for(size_t i = 0; i < choices.size(); ++i) {
				duel.SetResponse(choices[i].response);
				Step cs = StepToPrompt();
				bool dead = (cs == Step::Rejected) || !SummonsOk(summons) ||
							material_violation;
				int64_t sc = -1;
				if(!dead) {
					ComputeBoardKeyInto(duel,
										static_cast<uint8_t>(cfg.target_player),
										child_board_scratch);
					sc = static_cast<int64_t>(Heuristic(child_board_scratch));
				}
				scored.push_back({ i, sc,
								   plan_index.count(choices[i].plan_key) != 0,
								   dead });
				arena.Restore();
			}
			arena.Pop();

			// Le repertoire DEPARTAGE, il ne domine pas. En prime additive il
			// ferait passer "fin du tour" — que la reference joue, donc connue —
			// devant une invocation qui, elle, avance vraiment. Or terminer le
			// tour est irreversible : le board est fige.
			std::stable_sort(scored.begin(), scored.end(),
							 [](const Scored& a, const Scored& b) {
								 if(a.score != b.score) return a.score > b.score;
								 return a.known > b.known;
							 });
			// Le premier tirage est glouton pur ; les suivants s'en ecartent de
			// plus en plus rarement a mesure qu'on descend le classement.
			size_t r = 0;
			while(r + 1 < scored.size() && scored[r + 1].score >= 0 &&
				  (next() & 3u) == 0)
				++r;
			if(scored[r].dead) {
				++stats.dead_ends;
				return hit;
			}
			pick = scored[r].index;
		}

		duel.SetResponse(choices[pick].response);
		path.push_back(choices[pick].response);
	}
	// Sortie par le HAUT de la boucle : le plafond de decisions a mordu, pas
	// l'espace (C2).
	++stats.edges_skipped;
	return hit;
}

void Search::RunRollouts(const BoardKey& t, const std::vector<PlanStep>& p,
						 uint32_t count, uint64_t seed) {
	// Le self de cette sonde est la ligne « reste » du profil : tout ce que les
	// sondes internes ne couvrent pas (softmax, politique, tables, PQ...).
	prof::Scope ps(prof::kSearch);
	target = t;
	plan = &p;
	plan_index.clear();
	for(size_t i = 0; i < p.size(); ++i)
		if(p[i].edge)
			plan_index.emplace(p[i].edge, i);
	start = std::chrono::steady_clock::now();
	solutions.clear();
	path.clear();
	novelty.Clear();

	arena.Push();     // point de reprise : la racine
	for(uint32_t r = 0; r < count && !BudgetExhausted(); ++r) {
		uint64_t rng = seed + r * 0x9e3779b97f4a7c15ull;
		if(!rng)
			rng = 1;
		path.clear();
		++stats.rollout_count;
		bool hit = Rollout(rng);
		arena.Restore();
		if(hit && !cfg.anytime && solutions.size() >= cfg.max_solutions)
			break;
	}
	arena.Pop();

	stats.ms = std::chrono::duration<double, std::milli>(
				   std::chrono::steady_clock::now() - start).count();
	stats.hit_node_limit = stats.nodes >= cfg.max_nodes;
	stats.exhausted = false;   // un tirage n'epuise jamais rien
	stats.novelty_atoms = novelty.Size();
}

void Search::PolicyRollout(uint64_t& rng, const Policy& pol, NrpaRun& run) {
	auto next = [&rng] {
		rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
		return rng;
	};
	++stats.rollout_count;
	path.clear();
	run.score = 0;
	run.steps.clear();
	run.flat.clear();
	// Ligne PLATE pour le minage EN LIGNE (session 14) : les decisions ABSORBEES
	// par une macro n'apparaissent pas dans `steps` — re-miner dessus fabriquerait
	// des macros de macros, qui avortent au premier pas (cf. NrpaRun::flat).
	const bool mine_flat = cfg.options_online != nullptr;
	// Compteurs initiaux : le finisseur echantillonne depuis un etat de recul
	// deja profond — contraintes d'invocation, coupure de tour et gradient de
	// resolutions doivent compter depuis le prefixe, pas depuis zero.
	uint32_t actions = 0, turns = cfg.initial_turns,
			 summons = cfg.initial_summons;
	uint64_t resolved = cfg.initial_resolved;
	uint32_t rp_prev = ResolveProgress(resolved);
	// SONDE DE REPETITION (session 16) : compte BRUT des invocations de chaque
	// carte surveillee dans CE tirage. Distinct de `ResolveProgress`, qui
	// plafonne a min_count et somme toutes les entrees — c'est ce plafonnement
	// et cette somme qui rendaient l'histogramme aveugle a « un Liger contre
	// trois ». `rep_active` : au moins une premiere invocation a eu lieu, donc
	// il y a des decisions d'apres a compter.
	uint32_t rep_seen[4] = { 0, 0, 0, 0 };
	bool rep_active = false;
	// Cartes surveillees deja comptees comme PROPOSEES dans ce tirage : un bit
	// par entree, pour separer « combien de decisions offraient la carte » de
	// « combien de tirages en ont vu au moins une ».
	uint64_t rep_offered = 0;
	// Cartes surveillees deja ACTIVEES dans ce tirage.
	uint64_t rep_activated = 0;
	// Zones deja comptees dans ce tirage : 8 bits par entree surveillee.
	uint32_t rep_zone_seen = 0;
	const bool probe_on = cfg.probe_repeat && ProbeCount() > 0;
	// La source du delta : `--watch` (invocations pures) quand il existe,
	// sinon l'ancien compteur de resolutions. Les deux sont empaquetes de la
	// meme facon, mais ils ne comptent PAS la meme chose — le premier ne
	// compte que des invocations, le second aussi des activations.
	const bool probe_watch = !cfg.probe_watch.empty();
	double novel_states = 0;
	// Decisions consecutives sans atome inedit, et le verdict differe de
	// l'elagage par nouveaute (cfg.novelty_rollout_cut). A drapeau eteint,
	// `stale` compte pour rien et rien ne change — comportement d'avant.
	uint32_t stale = 0;
	bool novelty_cut = false;
	std::vector<double> logit;
	// Macro en cours d'execution (chantier 17) : les cles restantes se jouent
	// sans echantillonner ni produire de PolicyStep. Les prompts FORCES
	// intermediaires (un seul choix) passent au travers : le corpus n'en
	// enregistre pas, les sequences minees n'en contiennent donc jamais.
	const std::vector<uint64_t>* active_macro = nullptr;
	size_t macro_pos = 0;
	std::vector<std::pair<uint32_t, uint32_t>> applicable;   // (macro, choix)
	// Decisions ENREGISTREES (choix multiples) depuis le debut du tirage :
	// l'unite dans laquelle les positions du corpus sont exprimees — la
	// fenetre de proposition des macros se compare a ce compteur, pas a la
	// profondeur en prompts (qui compte aussi les coups forces).
	uint32_t nsteps = 0;
	// Le descripteur de contexte coute une requete de zone par decision : il ne
	// se calcule que si quelqu'un le consomme — le niveau contextuel de la
	// politique, la garde semantique du catalogue COURANT, ou (minage en ligne)
	// celle du catalogue A VENIR, dont les contextes se relevent maintenant. Ce
	// troisieme terme est le seul nouveau : sans lui, un run parti nu releverait
	// des contextes tous nuls et la garde semantique naitrait aveugle.
	const bool want_ctx = cfg.ctx_shrink >= 0.0f ||
						  (cfg.options && cfg.options->ctx_tol >= 0) ||
						  (mine_flat && cfg.options_online->ctx_tol >= 0);
	// CONDITIONNEMENT PAR LE CHEMIN (--mcps, REFUTE 9.21 (k)) : somme melangee
	// des coups joues sur les `mcps_depth` premieres decisions ENREGISTREES,
	// figee au-dela. Nul et jamais lu quand le mecanisme est eteint.
	uint64_t path_ctx = 0;
	// BANDIT DE TETE (--qhat) : cle du noeud courant, somme melangee des coups
	// DEJA joues. Distinct de `path_ctx` — il porte le coup EFFECTIVEMENT choisi
	// (l'id de la macro quand une macro est prise, pas sa premiere cle) et il
	// n'est pas fige au-dela de k, il cesse simplement d'etre consulte.
	uint64_t qh_ctx = 0;
	const bool qhat_on = cfg.qhat_depth != 0;
	// GARDE RAII : PolicyRollout sort par une douzaine de `return` — impasse,
	// terminal, contrainte violee, garde, borne brulees, budget — et ce sont
	// justement les tirages MORTS dont Q^ tire son signal. Un versement ecrit a
	// la main a chaque sortie en aurait oublie un, en silence.
	struct QhatGuard {
		Search* self;
		bool armed;
		~QhatGuard() { if(armed) self->QhatCommit(); }
	} qhat_guard{ this, qhat_on };
	// HINDSIGHT (chantier 2) : buts de substitution atteints par CE tirage,
	// (code, profondeur de la premiere invocation). Garde RAII pour la meme
	// raison que Q^ — les sorties sont trop nombreuses pour un versement ecrit
	// a la main, et ce sont justement les tirages MORTS qui portent le signal.
	static thread_local std::vector<std::pair<uint32_t, uint32_t>> hs_hits;
	const bool hs_on = cfg.hindsight > 0.0f;
	hs_hits.clear();
	struct HindsightGuard {
		Search* self;
		const NrpaRun* run;
		const std::vector<std::pair<uint32_t, uint32_t>>* hits;
		bool armed;
		~HindsightGuard() { if(armed) self->HindsightCommit(*run, *hits); }
	} hs_guard{ this, &run, &hs_hits, hs_on };
	// Les trois mecanismes qui lisent l'instantane du graphe (chantiers 1, 3, 4).
	const bool rec_on =
		cfg.recipes && (cfg.assign || cfg.backward || cfg.recipe_weight > 0.0f);
	if(qhat_on) {
		qh_nodes.clear();
		qh_moves.clear();
		qh_back.clear();
		qh_reward = 0.0;
	}

	// ELISION DES COUPS FORCES DANS LES TIRAGES (session 17) — la ou le cout est
	// REELLEMENT paye.
	//
	// MESURE QUI LE JUSTIFIE : 74,6 % des noeuds developpes n'offrent QU'UNE
	// reponse legale (attribution globale, --growth). Pour chacun d'eux, un
	// tirage paie aujourd'hui le prix fort — `ComputeBoardKeyInto` (deux
	// requetes de zone), `CollectAtoms` (quatre requetes et ~60-80 sondes de
	// table), l'heuristique, l'archive, le score — pour un point ou il n'y a
	// RIEN A DECIDER et ou la politique n'apprend rien (aucun PolicyStep n'est
	// produit sur un prompt a choix unique).
	//
	// `depth` cesse donc d'etre l'indice de boucle : il compte les DECISIONS
	// REELLES, et `pi` les prompts. Consequence a dire — `--max-decisions` change
	// de sens sous le drapeau, donc les profondeurs ne se comparent au temoin
	// qu'a budget de TEMPS egal.
	//
	// CE QUI EST CONSERVE SUR UN PROMPT ELIDE, et ce n'est pas negociable : la
	// comptabilite d'actions, de tours, d'invocations et de resolutions. Une
	// chaine forcee RESOUT des effets et peut poser une carte ; sauter cela
	// fausserait les contraintes et le but. Seul le travail d'EVALUATION est
	// saute.
	uint32_t depth = 0;
	const uint32_t prompt_cap =
		cfg.elide_forced ? cfg.max_decisions * 8u + 64u : cfg.max_decisions;
	for(uint32_t pi = 0; pi < prompt_cap && depth < cfg.max_decisions; ++pi) {
		if(BudgetExhausted()) {
			stats.hit_time_limit = true;
			return;
		}
		Step st = StepToPrompt();
		actions += actions_this_step;
		turns += turns_this_step;
		if(st == Step::Rejected) { ++stats.dead_ends; return; }
		if(st == Step::Ended)    { ++stats.terminals; return; }
		// Une contrainte violee tue le tirage avec son score courant : la
		// politique apprend d'elle-meme a la respecter.
		if(!SummonsOk(summons) || material_violation) {
			++stats.constraint_cuts;
			return;
		}
		// HINDSIGHT : les monstres d'EXTRA DECK reellement invoques par ce
		// tirage. Le filtre est un TYPE, pas une liste : aucun nom de carte n'est
		// compile, et l'ensemble ainsi designe est exactement celui dont l'arite
		// est le mur (une Fusion, une Synchro, un Xyz, un Lien consomment des
		// materiaux ; un monstre normal, non).
		if(hs_on)
			for(uint32_t raw : summons_this_step) {
				if(!raw)
					continue;   // invoquee face verso : code inconnu
				const uint32_t code = duel.Db().Canonical(raw);
				const CardRow* row = duel.Db().Find(code);
				if(!row || !(row->type & (TYPE_FUSION | TYPE_SYNCHRO | TYPE_XYZ |
										  TYPE_LINK)))
					continue;
				bool known = false;
				for(const auto& p : hs_hits)
					if(p.first == code) { known = true; break; }
				if(!known)
					hs_hits.emplace_back(code, depth);
			}
		summons += static_cast<uint32_t>(summons_this_step.size());
		resolved += resolved_this_step;
		++stats.nodes;
		prof::Count(prof::kDecisions);
		// --- SONDE : LES VOLETS QUI NE DEPENDENT PAS DU BOARD ---------------
		// PLACES ICI, AVANT L'ELISION, et c'est un correctif et non un detail :
		// un prompt elide ne redescend pas dans le corps de la boucle, et
		// `watch_*_this_step` est remis a zero au StepToPrompt suivant. Compter
		// plus bas ferait donc DISPARAITRE tout ce qu'une chaine forcee a
		// deplace ou active — c'est-a-dire l'essentiel, puisque 74,6 % des
		// prompts sont forces. Une sonde qui perd ses evenements sous un drapeau
		// rendrait un « jamais » qui n'existe pas.
		if(probe_on) {
			// PRESENCE EN ZONE : un tirage compte UNE FOIS par zone atteinte.
			if(watch_zone_this_step)
				for(size_t i = 0; i < ProbeCount(); ++i)
					for(int z = 0; z < 6; ++z) {
						const uint32_t bit = 1u << (8 * i + z);
						if(!(watch_zone_this_step & bit) ||
						   (rep_zone_seen & bit))
							continue;
						rep_zone_seen |= bit;
						++stats.rep[i].zone_rollouts[z];
					}
			// ACTIVATIONS, strictement observationnelles.
			if(watch_act_this_step)
				for(size_t i = 0; i < ProbeCount(); ++i) {
					const uint32_t a = static_cast<uint32_t>(
						(watch_act_this_step >> (16 * i)) & 0xffff);
					if(!a)
						continue;
					stats.rep[i].act_total += a;
					if(!(rep_activated & (1ull << i))) {
						rep_activated |= 1ull << i;
						++stats.rep[i].act_rollouts;
					}
				}
		}
		// --- ELISION : le prompt est-il FORCE ? ---------------------------
		// Enumere ICI, avant tout travail d'evaluation. La comptabilite
		// ci-dessus (actions, tours, invocations, resolutions) est deja faite :
		// un prompt elide reste compte, seule son EVALUATION est sautee.
		bool ro_filled = false;
		if(cfg.elide_forced) {
			if(!FillChoices(ro_choices)) {
				++stats.dead_ends;
				return;
			}
			ro_filled = true;
			CountOffers(rep_offered);
			if(ro_choices.size() == 1) {
				++stats.elided;
				duel.SetResponse(ro_choices[0].response);
				// NI `depth`, NI PolicyStep, NI score : rien ne s'est decide.
				// Un prompt a choix unique ne produisait deja aucun PolicyStep —
				// la politique n'y perd donc strictement rien.
				continue;
			}
		}
		// Histogramme des resolutions atteintes par les tirages : monotone le
		// long d'un tirage, chaque seuil n'est franchi qu'une fois.
		if(resolved_this_step) {
			const uint32_t rp = ResolveProgress(resolved);
			for(uint32_t k = rp_prev; k < rp; ++k) {
				if(k < 4)
					++stats.resolve_reached[k];
				else
					++stats.resolve_overflow;   // histogramme tronque (4.10)
			}
			rp_prev = rp;
		}

		ComputeBoardKeyInto(duel, static_cast<uint8_t>(cfg.target_player),
							board_scratch);
		const BoardKey& here = board_scratch;
		// INSTANTANE DU GRAPHE DE RECETTES (session 17) : la seule frontiere sure
		// est la profondeur 0 — aucun prompt n'est enumere, donc rien ne tient
		// l'adresse de `snap_useful`, que --assign sert a l'enumerateur.
		if(rec_on && depth == 0 && stats.rollout_count >= recipe_snap_next) {
			recipe_snap_next = stats.rollout_count + cfg.recipe_snap_period;
			RecipeSnapshot();
		}
		// --- SONDE DE REPETITION (session 16) ---
		// Placee ICI et pas au bloc des resolutions ci-dessus : elle a besoin du
		// board, qui vient d'etre calcule. Trois releves, dans l'ordre.
		if(probe_on) {
			// 1. La REFERENCE, une fois par worker : combien d'invocations pour
			//    un PREMIER exemplaire, depuis l'etat de depart des tirages.
			//    Sans elle, la distance mesuree plus loin est un nombre nu.
			if(depth == 0 && stats.rollout_count >= rep_d0_next) {
				rep_d0_next = stats.rollout_count + kRepD0Period;
				for(size_t i = 0; i < ProbeCount(); ++i) {
					uint32_t d = 0;
					const float r0 =
						RecipeDistance(here, resolved, ProbeCode(i), &d);
					stats.rep[i].code = ProbeCode(i);
					stats.rep[i].known = cfg.recipes->Knows(ProbeCode(i));
					stats.rep[i].d0 = d;
					stats.rep[i].rest0 = static_cast<uint32_t>(r0);
					++stats.rep[i].d0_samples;
				}
			}
			// 2. L'HISTOGRAMME par tirage, en compte BRUT et par entree.
			const uint64_t probe_delta =
				probe_watch ? watch_this_step : resolved_this_step;
			if(probe_delta) {
				for(size_t i = 0; i < ProbeCount(); ++i) {
					const uint32_t d = static_cast<uint32_t>(
						(probe_delta >> (16 * i)) & 0xffff);
					if(!d)
						continue;
					for(uint32_t k = rep_seen[i]; k < rep_seen[i] + d && k < 5;
						++k)
						++stats.rep[i].reached[k];
					const bool was_first = rep_seen[i] == 0;
					rep_seen[i] += d;
					// 3. A la PREMIERE invocation : les distances. Un balayage
					//    de zones, une seule fois par tirage et par produit.
					if(was_first) {
						RepeatProbeFirst(i, here, resolved, depth);
						rep_active = true;
					}
				}
			}
			// Decisions d'APRES : « le second n'arrive jamais » ne vaut que si le
			// tirage avait encore des decisions devant lui.
			if(rep_active)
				for(size_t i = 0; i < ProbeCount(); ++i)
					if(rep_seen[i])
						++stats.rep[i].after_sum;
		}
		if(GoalCheck(here, depth, actions, resolved)) {
			// Atteindre le board domine tout ; le cout departage en
			// LEXICOGRAPHIQUE — brulees d'abord, puis actions, puis decisions
			// (les unites 1e9/1e5/1 restent disjointes : brulees <= ~55,
			// actions <= ~1000, decisions <= ~500 ; tout reste entier exact
			// en double). L'adaptation NRPA tire donc vers la ligne la moins
			// CHERE, pas vers la premiere venue.
			const double gs = 1e12 - static_cast<double>(goal_burned) * 1e9 -
							  static_cast<double>(goal_actions) * 1e5 -
							  static_cast<double>(depth);
			if(gs > run.score)
				run.score = gs;
			// Recompense du bandit : le but vaut exactement 1, le maximum de
			// l'echelle. C'est le seul point ou elle ne se derive pas du
			// materiel — un board atteint n'est pas « beaucoup de materiel ».
			if(qhat_on)
				qh_reward = 1.0;
			if(!cfg.anytime)
				return;
			// Anytime : la ligne CONTINUE — des decisions de plus peuvent
			// reduire les brulees (recuperations reelles), et chaque
			// re-atteinte du board re-enregistre si elle est moins chere.
		}
		if(turns >= 2) {
			++stats.turn_cuts;
			break;
		}
		if(GuardCut(here, summons))
			return;
		// Borne brulees (B&B anytime) : un etat qui brule deja plus que la
		// meilleure ligne connue + la marge de recuperation ne la battra pas.
		if(const uint32_t bcut = EffectiveBurnCut();
		   bcut != UINT32_MAX && CurrentBurned() > bcut) {
			++stats.burn_cuts;
			return;
		}
		ArchiveObserve(here, depth, resolved);

		// --- SESSION 17 : LE GRAPHE DE RECETTES SUR LE CHEMIN CHAUD ---------
		// Une seule evaluation pour les chantiers 3 (distance) et 4 (progres a
		// rebours) : le cout EST le releve de presence, le refaire le doublerait.
		uint32_t rec_rem = 0, rec_back = 0;
		if(rec_on && (cfg.recipe_weight > 0.0f || cfg.backward)) {
			rec_rem = RecipeEval(here, cfg.backward ? &rec_back : nullptr);
			// La reference qui transforme la distance en PROGRES. Prise a la
			// premiere decision du tirage : tous les tirages d'une meme phase
			// partent du meme etat, donc c'est une constante de la phase et non
			// un denominateur qui bouge sous les comparaisons.
			if(depth == 0)
				recipe_base = rec_rem;
			stats.rec_roll_sum += rec_rem;
			stats.rec_roll_d0_sum += recipe_base;
			++stats.rec_roll_count;
			if(cfg.backward) {
				stats.backward_sum += rec_back;
				++stats.backward_count;
			}
		}

		// Le score d'un tirage est le MAX le long de la ligne, pas l'etat
		// final : un tirage qui a approche le board puis s'est ecrase reste un
		// meilleur guide qu'un tirage qui n'a jamais rien pose. La nouveaute
		// departage : entre deux lignes de meme materiel, celle qui a visite
		// des faits inedits merite l'adaptation.
		{
			// GATE. `--novelty 0` eteignait NoveltyCut (chemins DFS) mais PAS ce
			// bloc : le terme `novel_states` restait dans le score et le cout
			// etait integralement paye — quatre requetes au core, quatre tris et
			// ~60-80 sondes de table PAR DECISION, pour un simple departage. Le
			// bras temoin « sans nouveaute » du controle A/B ne couvrait donc que
			// les passes LDS, jamais NRPA (3.7). C'est aussi la principale
			// allocation NON BORNEE du run : `seen` croit sans limite, par worker.
			if(cfg.novelty_patience) {
				uint32_t partition = cfg.novelty_serialize
					? CommonCodes(here.codes, target.codes) * 16u +
						  ResolveProgress(resolved)
					: 0u;
				// SERIALISATION A REBOURS (chantier 4). La partition actuelle
				// compte les cartes CIBLES posees : elle est plate sur toute la
				// montee, precisement la ou l'arite tue le solveur. Le nombre de
				// sous-produits de la decomposition ET/OU deja fabriques, lui,
				// bouge des la premiere brique — la table de nouveaute se rouvre
				// donc AVANT qu'aucune carte cible n'existe. Serialized IW /
				// BFWS, applique a la decomposition apprise et non au but
				// litteral.
				if(cfg.backward)
					partition = partition * 64u + (rec_back & 63u);
				CollectAtoms(duel, static_cast<uint8_t>(cfg.target_player), here,
							 partition, atoms_scratch);
				if(novelty.Observe(atoms_scratch, depth)) {
					++stats.novelty_novel;
					novel_states += 1;
					stale = 0;
				} else {
					++stats.novelty_stale;
					// ELAGAGE PAR NOUVEAUTE DANS LES TIRAGES SOUS POLITIQUE
					// (repare session 15). `novelty_rollout_cut` etait cable
					// dans Rollout() — le tirage GLOUTON, qui ne fait plus rien
					// — et JAMAIS ici, alors que le verdict de nouveaute y est
					// deja CALCULE a chaque decision (quatre requetes au core,
					// ~60-80 sondes de table) et jete apres un simple
					// departage. Le mecanisme le plus cher du tirage ne servait
					// qu'a briser des egalites de score.
					// La coupure est differee a la fin du bloc : le score de CE
					// point compte quand meme, sans quoi couper ferait perdre du
					// materiel deja atteint.
					if(cfg.novelty_rollout_cut &&
					   ++stale > cfg.novelty_patience)
						novelty_cut = true;
				}
			}
			// Les resolutions exigees (--resolve) pesent PLUS que des cartes
			// cibles (cfg.resolve_weight) : a poids egal, les lignes 8/8 sans
			// rip gagnaient la course d'adaptation contre les rip-partielles.
			double material = static_cast<double>(Heuristic(here)) +
							  static_cast<double>(cfg.resolve_weight) *
								  ResolveProgress(resolved);
			double sc = material * 1000.0 + novel_states;
			// LANDMARKS APPRIS (chantier 18) : le `h` qui DECROIT, servi la ou
			// il peut changer quelque chose. Chaque accomplissement de landmark
			// vaut `landmark_weight` points de materiel — meme unite que
			// `resolve_weight`, donc reglable dans la meme echelle.
			//
			// EN PROGRES ET NON EN DISTANCE, et c'est ce qui compte : le score
			// des tirages est un « plus haut vaut mieux », donc on compte les
			// landmarks ACCOMPLIS, pas ceux qui restent. Poser « Leo Dancer au
			// cimetiere » une deuxieme fois fait monter le score AVANT qu'aucun
			// Liger n'existe — c'est exactement ce que le `h` plat ne sait pas
			// faire, et la raison pour laquelle une ligne qui prepare deux
			// Ligers paraissait pire qu'une ligne qui en pose un.
			//
			// HORS de `material` : `material` sert aussi de recompense au
			// bandit Q^, normalisee par le materiel du board cible. Y verser des
			// points de landmark changerait cette echelle et rendrait les
			// mesures de la session 15 incomparables.
			if(cfg.landmark_weight > 0.0f && cfg.landmarks) {
				const uint32_t total =
					static_cast<uint32_t>(cfg.landmarks->Items().size());
				const uint32_t rem = LandmarkRemaining(here);
				sc += static_cast<double>(cfg.landmark_weight) * 1000.0 *
					  static_cast<double>(total - rem);
			}
			// DISTANCE DE RECETTES (chantier 3) : le `h` qui DECROIT pendant
			// qu'on construit, servi la ou il peut changer quelque chose. C'est
			// le seul terme du score qui bouge AVANT qu'une carte cible ne soit
			// posee : `common` compte les cartes cibles PRESENTES et vaut donc
			// zero sur toute la montee, ce qui est la cause mecanique de la loi
			// d'arite (9.23 (h)).
			//
			// EN PROGRES ET NON EN DISTANCE, et ce n'est pas cosmetique : le
			// score d'un tirage est un MAX initialise a 0, donc un terme negatif
			// ne remonterait jamais au-dessus et le gradient serait mort. On
			// compte donc `d0 - d`, ce que la ligne a GAGNE depuis son depart.
			// HORS de `material` pour la meme raison que les landmarks : cette
			// variable sert aussi de recompense au bandit Q^, et changer son
			// echelle rendrait les mesures de la session 15 incomparables.
			if(cfg.recipe_weight > 0.0f && rec_on) {
				const double gained = recipe_base > rec_rem
										  ? double(recipe_base - rec_rem)
										  : 0.0;
				sc += static_cast<double>(cfg.recipe_weight) * 1000.0 * gained;
			}
			if(sc > run.score)
				run.score = sc;
			// Recompense du bandit (--qhat) : le MEME materiel, rapporte a
			// celui du board cible. Bornee, comparable entre runs, et
			// SANS la nouveaute — qui est un departage de gradient, pas une
			// mesure de reussite, et qui n'est pas bornee.
			if(qhat_on && qhat_scale > 0.0) {
				const double r = material / qhat_scale;
				if(r > qh_reward)
					qh_reward = r;
			}
			if(novelty_cut) {
				++stats.novelty_cuts;
				return;
			}
		}

		ChoiceList& choices = ro_choices;
		// Deja rempli par le test d'elision : le refaire couterait une seconde
		// enumeration par decision, c'est-a-dire exactement ce qu'on economise.
		if(!ro_filled && !FillChoices(choices)) {
			++stats.dead_ends;
			return;
		}
		// SONDE D'OFFRE : compte SEULEMENT si l'elision ne l'a pas deja fait,
		// sinon les prompts non forces seraient comptes deux fois.
		if(!ro_filled)
			CountOffers(rep_offered);

		size_t pick = 0;
		bool scripted = false;
		// Macro active : jouer sa cle suivante si CE prompt (a choix multiple)
		// la propose ; sinon elle AVORTE et le tirage reprend son cours — le
		// mecanisme ne retire jamais rien de l'espace, au pire il ne fait rien.
		if(active_macro && choices.size() > 1) {
			const uint64_t want = (*active_macro)[macro_pos];
			for(size_t i = 0; i < choices.size(); ++i)
				if(choices[i].plan_key == want) {
					pick = i;
					scripted = true;
					break;
				}
			if(scripted) {
				++stats.macro_absorbed;
				if(++macro_pos >= active_macro->size())
					active_macro = nullptr;
				// La decision ABSORBEE entre quand meme dans la ligne plate :
				// c'est precisement celle que `steps` ne verra jamais, et celle
				// qu'un re-minage doit pouvoir traverser pour rallonger la macro
				// au lieu d'en fabriquer une de macros.
				if(mine_flat) {
					PolicyStep f;
					f.keys.reserve(choices.size());
					for(const Choice& c : choices)
						f.keys.push_back(c.plan_key);
					// Une decision absorbee n'a jamais ete echantillonnee : ses
					// biais ne sont pas definis. Les vecteurs sont dimensionnes
					// (tout consommateur qui les indexe reste sur pied) et nuls ;
					// le mineur, lui, ne lit que `keys`, `chosen` et `ctx`.
					f.known.assign(choices.size(), 0);
					f.hinted.assign(choices.size(), 0);
					f.chosen = pick;
					if(want_ctx)
						f.ctx = ContextKey(
							CommonCodes(here.codes, target.codes),
							duel.Count(static_cast<uint8_t>(cfg.target_player),
									   LOCATION_HAND));
					run.flat.push_back(std::move(f));
				}
			} else {
				++stats.macro_aborted;
				active_macro = nullptr;
			}
		}
		if(!scripted && choices.size() > 1) {
			// Echantillonnage softmax sous la politique — AUCUNE evaluation des
			// fils : c'est ce qui rend un tirage NRPA plusieurs fois moins cher
			// qu'un tirage glouton (qui avance/mesure/restaure chaque fils).
			PolicyStep step;
			// CONTEXTE de la decision (chantier 5ter) : cartes du board cible
			// deja posees. Le meme descripteur que celui releve sur les lignes
			// de corpus — il est SEMANTIQUE, donc comparable d'une ligne a
			// l'autre, contrairement a la profondeur.
			// Le descripteur coute une requete de zone par decision : il ne se
			// paie que si le niveau contextuel est allume — ou si la garde
			// semantique des options en a besoin.
			if(want_ctx)
				step.ctx = ContextKey(
					CommonCodes(here.codes, target.codes),
					duel.Count(static_cast<uint8_t>(cfg.target_player),
							   LOCATION_HAND));
			// CONDITIONNEMENT EFFECTIF du niveau contextuel : le CHEMIN sous
			// MCPS (les coups deja joues sur cette ligne), le descripteur
			// semantique sinon. `path_ctx` est le chemin AVANT cette decision —
			// c'est bien « la racine jusqu'a ce noeud », le coup courant n'en
			// fait pas partie.
			step.cctx = cfg.mcps_depth ? path_ctx
									   : static_cast<uint64_t>(step.ctx);
			step.keys.reserve(choices.size());
			step.known.reserve(choices.size());
			step.hinted.reserve(choices.size());
			logit.resize(choices.size());
			double mx = -1e300;
			for(size_t i = 0; i < choices.size(); ++i) {
				uint64_t key = choices[i].plan_key;
				// Les changements de phase sont au repertoire (la reference
				// finit son tour) mais n'y meritent aucun biais : avec +1,5 un
				// prompt idle a ~10 choix terminerait le tour une fois sur
				// trois, et terminer le tour est irreversible. La politique
				// reste libre de l'apprendre — seul le PRIOR est retire.
				// (Drapeau `phase`, pas le label : les chemins chauds n'ont
				// plus de labels.)
				bool known = plan_index.count(key) != 0 && !choices[i].phase;
				// Indice de domaine : ce coup engage une carte designee par
				// --hint, il part avec une prime d'echantillonnage.
				bool hinted = choices[i].card && !cfg.hint_cards.empty() &&
							  std::find(cfg.hint_cards.begin(),
										cfg.hint_cards.end(),
										choices[i].card) != cfg.hint_cards.end();
				// BIAIS D'ASSIGNATION (--assign-bias) : ce choix engage-t-il un
				// MATERIAU designe par le graphe de recettes ? La liste vient de
				// `snap_useful`, derivee des recettes et non de la cible
				// litterale — c'est ce qui la distingue de `--goal-bias`, qui
				// biaisait vers un coup n'existant pas encore. Le goulot mesure
				// est ici : Leo Dancer est offert 14 433 fois dans le prompt
				// « quel Lunalight envoyer au cimetiere » et choisi 202 fois.
				const bool useful =
					cfg.assign_bias > 0.0f && choices[i].card &&
					!snap_useful.empty() &&
					std::binary_search(snap_useful.begin(), snap_useful.end(),
									   choices[i].card);
				double w = EffectiveWeight(pol, &ctx_weights, key, step.cctx,
										   cfg.ctx_shrink);
				logit[i] = (w + (known ? cfg.nrpa_bias_known : 0.0f) +
							(hinted ? cfg.hint_bias : 0.0f) +
							(useful ? cfg.assign_bias : 0.0f)) /
						   (cfg.nrpa_temp > 1e-3f ? cfg.nrpa_temp : 1e-3f);
				mx = (std::max)(mx, logit[i]);
				step.keys.push_back(key);
				step.known.push_back(known ? 1 : 0);
				// Bit 0 = --hint, bit 1 = materiau utile (--assign-bias). Les
				// DEUX doivent etre rejoues par AdaptRun : un gradient calcule
				// sous une distribution qui n'est pas celle de l'echantillonnage
				// est faux, et c'est exactement la faute que l'audit 7.5 avait
				// trouvee sur hint_bias.
				step.hinted.push_back(static_cast<uint8_t>((hinted ? 1 : 0) |
														   (useful ? 2 : 0)));
			}
			// OPTIONS (chantier 17) : une macro applicable devient un choix de
			// plus, pesee par SON poids — l'unite d'echantillonnage qui
			// concentre la masse. Deux gardes, posees par le premier A/B
			// (9.19 (g)) : UNE macro par premiere cle (la mieux classee —
			// l'index suit l'ordre du minage), sans quoi le catalogue inonde
			// le softmax (7,7 prises/tirage mesurees) ; et AUCUN biais herite
			// (le poids part de zero, l'adaptation seule les eleve — avec le
			// biais repertoire, 82 % des prises avortaient apres ~1,3 pas).
			applicable.clear();
			if(cfg.options) {
				const uint32_t W = cfg.options->window;
				for(size_t i = 0; i < choices.size(); ++i) {
					auto it = cfg.options->by_first.find(choices[i].plan_key);
					if(it == cfg.options->by_first.end())
						continue;
					// Preconditions : fenetre de position (refutee, gardee
					// pour l'A/B) et garde SEMANTIQUE (ctx compatible avec
					// une occurrence du corpus) — les memes tests que le
					// modele de selection. La "meilleure macro par premiere
					// cle" devient la mieux classee PARMI LES COMPATIBLES.
					for(uint32_t mi : it->second) {
						const uint32_t p = cfg.options->pos[mi];
						if(W && (nsteps + W < p || nsteps > p + W))
							continue;
						if(!cfg.options->CtxOk(mi, step.ctx))
							continue;
						applicable.emplace_back(mi, static_cast<uint32_t>(i));
						break;   // une macro par premiere cle
					}
				}
				for(const auto& [mi, ci] : applicable) {
					const double w =
						EffectiveWeight(pol, &ctx_weights, cfg.options->ids[mi],
										step.cctx, cfg.ctx_shrink);
					logit.push_back(
						w / (cfg.nrpa_temp > 1e-3f ? cfg.nrpa_temp : 1e-3f));
					mx = (std::max)(mx, logit.back());
					step.keys.push_back(cfg.options->ids[mi]);
					step.known.push_back(0);
					step.hinted.push_back(0);
				}
			}
			size_t pick_index = 0;
			// BANDIT DE TETE (--qhat) : sur les k premieres decisions
			// ENREGISTREES, la regle de selection de MCPS remplace le softmax.
			// Elle est GLOUTONNE (argmax), comme dans le papier : la diversite
			// vient du tirage NRPA en dessous, pas d'un bruit ajoute ici — et
			// les moyennes bougent a chaque tirage, y compris quand il meurt.
			bool by_bandit = false;
			if(qhat_on && nsteps < cfg.qhat_depth) {
				BanditNode* node = nullptr;
				auto in = bandit.find(qh_ctx);
				if(in != bandit.end()) {
					node = &in->second;
				} else if(bandit.size() < cfg.qhat_max_nodes) {
					node = &bandit[qh_ctx];
					// La CONDITION de Q^ a ce noeud : les coups du CHEMIN,
					// c'est-a-dire ceux que le bandit a lui-meme choisis
					// (`qh_back`), et non tout ce que le tirage a joue depuis.
					// Les decisions absorbees par une macro n'en font pas
					// partie : l'arete de l'arbre est la MACRO, ses pas sont sa
					// consequence — les inclure retrecirait l'ensemble
					// conditionnant sans rien conditionner de plus.
					node->cond.reserve(qh_back.size());
					for(const auto& [nk, mk] : qh_back)
						node->cond.push_back(mk);
					std::sort(node->cond.begin(), node->cond.end());
					node->cond.erase(
						std::unique(node->cond.begin(), node->cond.end()),
						node->cond.end());
				} else {
					++stats.qhat_fallback;
				}
				if(node) {
					qh_nodes.push_back(qh_ctx);
					// REFERENCE DE PERMUTATION : le noeud GELE le plus profond
					// du chemin (le s_r du papier, propage au sous-arbre).
					// Aucun -> la racine, dont la statistique est tenue
					// incrementalement et toujours fraiche.
					BanditNode* sr = nullptr;
					for(size_t i = qh_nodes.size(); i-- > 0;) {
						auto it = bandit.find(qh_nodes[i]);
						if(it != bandit.end() && it->second.frozen) {
							sr = &it->second;
							break;
						}
					}
					double best_val = -1e300;
					uint32_t ties = 0;
					// ETIQUETAGE POUR LA SONDE, aux tout premiers passages sur
					// ce noeud seulement (`q` se remplit des la premiere
					// retropropagation) : la sonde doit pouvoir NOMMER les
					// coups, y compris ceux que le bandit n'a jamais choisis —
					// ce sont justement eux que Q^ departage. L'etiqueter a
					// chaque visite couterait une sonde de table par candidat.
					const bool label_here =
						cfg.enumeration.db && node->q.empty();
					for(size_t i = 0; i < step.keys.size(); ++i) {
						const uint64_t key = step.keys[i];
						if(label_here && i < choices.size())
							bandit_code.emplace(key, choices[i].card);
						PermStat ph;
						if(sr) {
							auto ic = sr->perm.find(key);
							if(ic != sr->perm.end()) {
								ph = ic->second;
							} else {
								ph = perm_win.Stat(key, sr->mask);
								sr->perm.emplace(key, ph);
							}
						} else {
							ph = perm_win.Root(key);
						}
						uint32_t n = 0;
						double w = 0;
						if(auto iq = node->q.find(key); iq != node->q.end()) {
							n = iq->second.first;
							w = iq->second.second;
						}
						// val = (n Q + n^ Q^) / (n + n^), avec n Q = w. Poids
						// PROPORTIONNELS AUX EFFECTIFS : c'est la combinaison
						// de variance minimale, et c'est elle qui supprime
						// l'hyperparametre de biais de GRAVE. Un coup jamais vu
						// (n + n^ = 0) passe devant tous les autres — la regle
						// de premiere visite du papier, qui garantit que chaque
						// ouverture est essayee au moins une fois.
						const double val =
							(n + ph.n) ? (w + double(ph.n) * double(ph.q)) /
											 double(n + ph.n)
									   : 2.0;
						// Ex aequo departages au hasard (reservoir) : sans
						// cela l'ordre d'ENUMERATION du core deciderait de
						// l'ouverture pendant toute la premiere passe, ou tout
						// est ex aequo a 2,0.
						if(val > best_val) {
							best_val = val;
							pick_index = i;
							ties = 1;
						} else if(val == best_val &&
								  (next() % ++ties) == 0) {
							pick_index = i;
						}
					}
					by_bandit = true;
					step.bandit = 1;
					++stats.qhat_decisions;
					if(!nsteps)
						++stats.qhat_first;
					qh_back.emplace_back(qh_ctx, step.keys[pick_index]);
					if(cfg.enumeration.db && pick_index < choices.size())
						bandit_code.emplace(step.keys[pick_index],
											choices[pick_index].card);
				}
			}
			if(!by_bandit) {
				double sum = 0;
				for(double& x : logit) { x = std::exp(x - mx); sum += x; }
				double u = double(next() >> 11) *
						   (1.0 / 9007199254740992.0) * sum;
				double acc = 0;
				for(size_t i = 0; i < logit.size(); ++i) {
					acc += logit[i];
					if(u < acc || i + 1 == logit.size()) {
						pick_index = i;
						break;
					}
				}
			}
			if(pick_index >= choices.size()) {
				// Macro choisie : la reponse appliquee est son premier coup,
				// le curseur prend la suite.
				const auto& [mi, ci] = applicable[pick_index - choices.size()];
				pick = ci;
				++stats.macro_taken;
				const std::vector<uint64_t>& seq = cfg.options->seqs[mi];
				if(seq.size() > 1) {
					active_macro = &seq;
					macro_pos = 1;
				}
				// La macro est un COUP a part entiere pour la fenetre : son id
				// est un code comme un autre, et c'est ainsi que Q^ peut la
				// departager de ses concurrentes atomiques a la meme decision.
				if(qhat_on)
					qh_moves.push_back(cfg.options->ids[mi]);
			} else {
				pick = pick_index;
			}
			// Le CHEMIN du bandit s'allonge du coup effectivement choisi (id de
			// macro compris). Au-dela de k il n'est plus consulte : inutile de
			// le tenir.
			if(qhat_on && nsteps < cfg.qhat_depth)
				qh_ctx += MixMove(step.keys[pick_index]);
			// Visibilite des indices : un coup indice etait-il seulement LEGAL
			// ici ? C'est la mesure qui separe "mal echantillonne" de "jamais
			// propose par le core".
			if(!cfg.hint_cards.empty()) {
				bool any = false;
				for(uint8_t h : step.hinted)
					any |= h != 0;
				if(any) {
					++stats.hint_seen;
					if(step.hinted[pick_index])
						++stats.hint_taken;
				}
			}
			// Ligne PLATE : la meme decision sous forme ATOMIQUE — les ids de
			// macro (la queue de step.keys) sont ecartes et le coup retenu est
			// le choix du PROMPT reellement joue, macro choisie comprise (`pick`
			// est alors son premier coup).
			if(mine_flat) {
				const size_t n = choices.size();
				PolicyStep f;
				f.ctx = step.ctx;
				f.chosen = pick;
				f.keys.assign(step.keys.begin(), step.keys.begin() + n);
				f.known.assign(step.known.begin(), step.known.begin() + n);
				f.hinted.assign(step.hinted.begin(), step.hinted.begin() + n);
				run.flat.push_back(std::move(f));
			}
			step.chosen = pick_index;
			run.steps.push_back(std::move(step));
		}

		if(choices.size() > 1) {
			// Le chemin s'allonge du coup ATOMIQUE reellement joue (macro
			// choisie comprise : c'est son premier coup) — et seulement sur les
			// `mcps_depth` premieres decisions. Au-dela il est fige : toutes les
			// decisions profondes partagent alors le contexte de leur prefixe,
			// ce qui BORNE la table au lieu d'y creer une case par noeud.
			if(cfg.mcps_depth && nsteps < cfg.mcps_depth)
				path_ctx += MixMove(choices[pick].plan_key);
			// Le coup ATOMIQUE joue entre dans le multiensemble du tirage, que
			// la decision ait ete echantillonnee, dictee par le bandit ou
			// ABSORBEE par une macro : « la partie contient a » ne se soucie ni
			// de l'ordre ni de qui a decide.
			if(qhat_on)
				qh_moves.push_back(choices[pick].plan_key);
			++nsteps;
		}
		// CONVERSION OFFRE -> CHOIX (session 17). Le coup retenu engage-t-il une
		// carte surveillee ? Apparie a `offer_steps`, cela donne le juge que la
		// rarete de « Leo au cimetiere » rendait inutilisable.
		if(probe_on && choices[pick].card)
			for(size_t i = 0; i < ProbeCount(); ++i)
				if(ProbeCode(i) == choices[pick].card)
					++stats.rep[i].taken_steps;
		duel.SetResponse(choices[pick].response);
		path.push_back(choices[pick].response);
		// UNE DECISION REELLE de plus. `depth` n'est plus l'indice de boucle :
		// il n'avance que sur les prompts qui offraient un choix, et c'est ce
		// qui rend `--max-decisions` comparable a la profondeur d'une ligne
		// « apres elision » (143 au lieu de 284 sur la reference).
		++depth;
	}
	// Sortie par le HAUT : plafond de decisions (C2).
	++stats.edges_skipped;
}

void Search::QhatCommit() {
	// Recompense du tirage, bornee. Elle vaut 0 pour un tirage qui n'a rien
	// pose — ce qui est le cas des tirages MORTS, et c'est tout l'interet : Q^
	// moyenne sur TOUS les tirages, pas sur les meilleurs.
	double r = qh_reward;
	if(r < 0.0) r = 0.0;
	if(r > 1.0) r = 1.0;
	++stats.qhat_playouts;
	stats.qhat_reward_sum += r;
	// 1. LA FENETRE GLISSANTE. Le multiensemble des coups du tirage, trie et
	//    dedoublonne : MCPS ne compte qu'une presence par partie.
	std::sort(qh_moves.begin(), qh_moves.end());
	qh_moves.erase(std::unique(qh_moves.begin(), qh_moves.end()),
				   qh_moves.end());
	perm_win.Push(qh_moves, static_cast<float>(r));
	// 2. Q(s,a) : retropropagation sur les couples (noeud, coup) traverses.
	for(const auto& [nk, mk] : qh_back) {
		auto it = bandit.find(nk);
		if(it == bandit.end())
			continue;
		auto& e = it->second.q[mk];
		++e.first;
		e.second += r;
	}
	// 3. VISITES ET GEL. Un noeud non racine qui atteint rho visites fige sa
	//    statistique de permutation et devient la reference de son sous-arbre.
	//    La racine (condition vide) ne gele jamais : elle est tenue
	//    incrementalement par la fenetre, donc toujours fraiche — et c'est elle
	//    qui porte le diagnostic de la premiere decision.
	for(uint64_t nk : qh_nodes) {
		auto it = bandit.find(nk);
		if(it == bandit.end())
			continue;
		BanditNode& nd = it->second;
		++nd.visits;
		if(!nd.frozen && !nd.cond.empty() && nd.visits >= cfg.qhat_rho) {
			nd.frozen = true;
			perm_win.Mask(nd.cond, nd.mask);
		}
	}
}

std::vector<BanditProbe> Search::RootBandit() const {
	std::vector<BanditProbe> out;
	if(!cfg.qhat_depth)
		return out;
	// L'UNION de deux ensembles, et c'est le point de la sonde. Les coups que
	// le bandit a CHOISIS a la racine (`q`) ne sont que les quatre ouvertures
	// du premier prompt — une decision triviale. Ce que le diagnostic vise est
	// Q^({}, a) pour TOUT coup a : la recompense moyenne des lignes qui l'ont
	// joue, n'importe ou et dans n'importe quel ordre. C'est exactement la
	// statistique de racine de la fenetre, et elle existe pour tous les codes
	// que les k premieres decisions ont proposes — la cible de Tenki comprise,
	// qui se decide au DEUXIEME prompt et que la table `q` de la racine ne
	// verrait jamais.
	std::unordered_map<uint64_t, BanditProbe> acc;
	if(auto it = bandit.find(0ull); it != bandit.end())
		for(const auto& [key, nw] : it->second.q) {
			BanditProbe& p = acc[key];
			p.key = key;
			p.n = nw.first;
			p.w = nw.second;
		}
	for(const auto& [key, code] : bandit_code) {
		BanditProbe& p = acc[key];
		p.key = key;
		p.code = code;
	}
	out.reserve(acc.size());
	for(auto& [key, p] : acc) {
		const PermStat ph = perm_win.Root(key);
		p.nhat = ph.n;
		p.qhat_sum = double(ph.n) * double(ph.q);
		if(p.n || p.nhat)
			out.push_back(p);
	}
	return out;
}

void AdaptRun(NrpaPolicy& pol, NrpaResidual* res, const NrpaRun& run,
			  float alpha, float bias_known, float hint_bias, float shrink,
			  float temp, size_t ctx_max, float assign_bias) {
	const double inv_t = 1.0 / (temp > 1e-3f ? temp : 1e-3f);
	const bool two_level = res && shrink >= 0.0f;
	// Le gradient contextuel CREE une case par (coup, contexte). Sous
	// conditionnement par le chemin, le nombre de contextes n'est plus borne par
	// 256 mais par le nombre de prefixes visites : sans plafond, la table
	// enflerait sans fin, par worker (meme famille que la table de nouveaute).
	// Au plafond, les cases existantes vivent leur vie, aucune neuve n'apparait.
	auto touch = [&](uint64_t k) -> CtxWeight* {
		auto it = res->find(k);
		if(it != res->end())
			return &it->second;
		if(ctx_max && res->size() >= ctx_max)
			return nullptr;
		return &(*res)[k];
	};
	std::vector<double> p;
	for(const PolicyStep& s : run.steps) {
		// Decision prise par le BANDIT DE TETE (--qhat) : elle n'a pas ete
		// tiree du softmax, donc le gradient NRPA n'y est pas defini. L'y
		// appliquer pousserait +alpha le coup choisi a chaque tirage sans le
		// moindre contrepoids — le mode d'echec exact de `--mcps` (9.21 (k)).
		if(s.bandit)
			continue;
		p.resize(s.keys.size());
		double mx = -1e300;
		for(size_t i = 0; i < s.keys.size(); ++i) {
			double w = EffectiveWeight(pol, res, s.keys[i], s.cctx, shrink);
			p[i] = (w + (s.known[i] ? bias_known : 0.0f) +
					(i < s.hinted.size() ? ((s.hinted[i] & 1) ? hint_bias : 0.0f) +
											   ((s.hinted[i] & 2) ? assign_bias : 0.0f)
										 : 0.0f)) *
				   inv_t;
			mx = (std::max)(mx, p[i]);
		}
		double sum = 0;
		for(double& x : p) { x = std::exp(x - mx); sum += x; }
		pol[s.keys[s.chosen]] += alpha;
		for(size_t i = 0; i < s.keys.size(); ++i)
			pol[s.keys[i]] -= static_cast<float>(alpha * p[i] / sum);
		if(!two_level)
			continue;
		// Le niveau contextuel recoit le MEME gradient, sur la case
		// (coup, contexte). Son compteur mesure l'evidence accumulee dans CE
		// contexte : c'est lui qui decide, via s(n), quand il prend la main.
		if(CtxWeight* c = touch(CtxKey(s.keys[s.chosen], s.cctx)))
			c->w += alpha;
		for(size_t i = 0; i < s.keys.size(); ++i) {
			CtxWeight* c = touch(CtxKey(s.keys[i], s.cctx));
			if(!c)
				continue;
			c->w -= static_cast<float>(alpha * p[i] / sum);
			++c->n;
		}
	}
}

void AdaptCorpus(NrpaPolicy& pol, NrpaResidual* res,
				 const std::vector<NrpaRun>& runs, uint32_t passes, float alpha,
				 float bias_known, float hint_bias, float shrink, float temp,
				 size_t ctx_max) {
	for(uint32_t pass = 0; pass < passes; ++pass)
		for(const NrpaRun& r : runs)
			AdaptRun(pol, res, r, alpha, bias_known, hint_bias, shrink, temp,
					 ctx_max);
}

void Search::Adapt(Policy& pol, const NrpaRun& best) {
	AdaptRun(pol, &ctx_weights, best, cfg.nrpa_alpha, cfg.nrpa_bias_known,
			 cfg.hint_bias, cfg.ctx_shrink, cfg.nrpa_temp, cfg.ctx_max,
			 cfg.assign_bias);
}

double Search::Nrpa(int level, const Policy& pol, NrpaRun& best, uint64_t& rng) {
	if(level <= 0) {
		best.score = -1;
		best.steps.clear();
		PolicyRollout(rng, pol, best);
		arena.Restore();
		return best.score;
	}
	// UNE copie de la politique par appel de niveau. Elle etait copiee a
	// chaque tirage (passage par valeur jusqu'au niveau 0) : une table de
	// milliers d'entrees dupliquee des dizaines de milliers de fois par run.
	Policy local = pol;
	best.score = -1;
	uint32_t stagnant = 0, repeats = 0;
	for(uint32_t i = 0; i < cfg.nrpa_iters; ++i) {
		if(BudgetExhausted() ||
		   (!cfg.anytime && solutions.size() >= cfg.max_solutions))
			break;
		NrpaRun child;
		Nrpa(level - 1, local, child, rng);
		if(child.score > best.score) {
			best = std::move(child);
			stagnant = 0;
			repeats = 0;
		} else if(cfg.nrpa_lr && MatScore(child.score) == MatScore(best.score)) {
			// Repetitions limitees (arXiv:2401.10420) : la meilleure ligne est
			// RE-TROUVEE (meme score materiel — la part nouveaute decroit a
			// chaque rejeu, l'egalite stricte ne se produirait jamais). C'est
			// la convergence qui se signale elle-meme : inutile d'attendre que
			// la stagnation l'admette, on rend la main tout de suite.
			if(++repeats >= cfg.nrpa_lr)
				break;
		} else if(++stagnant >= 8) {
			// La politique rejoue la meme ligne sans plus progresser : c'est le
			// mode de defaillance documente de NRPA (convergence prematuree).
			// Adapter davantage ne ferait que la figer — on rend la main, le
			// niveau au-dessus ou un redemarrage relancera la diversite.
			break;
		}
		if(best.score >= 0) {
			Adapt(local, best);
			++stats.nrpa_adapts;
		}
	}
	return best.score;
}

void Search::OnlineOptionsTick(const NrpaRun& best) {
	OnlineOptions* oo = cfg.options_online;
	if(!oo)
		return;
	const auto now = std::chrono::steady_clock::now();
	bool mine = false;
	std::vector<NrpaRun> corpus;
	// Parametres de minage recopies SOUS VERROU : ils ne changent jamais apres
	// le demarrage, mais les lire hors verrou serait une course a la lettre du
	// standard, et ce fichier ne laisse pas de course « benigne » derriere lui.
	size_t m_max = 0, m_len = 0;
	uint32_t m_sup = 0, m_win = 0;
	int m_ctx = -1;
	{
		std::lock_guard<std::mutex> lock(oo->mu);
		// 1. CONTRIBUTION. La ligne plate de la meilleure sequence de ce worker
		// entre au corpus vivant. Trois filtres, dans cet ordre : elle doit
		// exister, etre INEDITE (les seize workers repartent tous de la meilleure
		// sequence partagee — sans dedoublonnage le corpus serait seize fois la
		// meme ligne), et respecter le quota de son worker.
		if(best.score > 0 && best.flat.size() >= 2) {
			++oo->offered;
			uint64_t sig = 0x9E3779B97F4A7C15ull;
			for(const PolicyStep& s : best.flat)
				sig = (sig ^ s.keys[s.chosen]) * 0x100000001B3ull;
			bool dup = false;
			for(const auto& e : oo->pool)
				if(e.sig == sig) { dup = true; break; }
			if(dup) {
				++oo->dups;
			} else {
				// Quota par worker : la place se prend a la PIRE ligne de ce
				// worker, pas a celle d'un autre. C'est ce qui empeche un worker
				// prolifique de vider le corpus de la diversite des autres.
				size_t mine_n = 0, worst = SIZE_MAX;
				for(size_t i = 0; i < oo->pool.size(); ++i)
					if(oo->pool[i].worker == cfg.worker_id) {
						++mine_n;
						if(worst == SIZE_MAX ||
						   oo->pool[i].score < oo->pool[worst].score)
							worst = i;
					}
				bool take = true;
				if(mine_n >= oo->per_worker) {
					if(oo->pool[worst].score >= best.score)
						take = false;
					else
						oo->pool.erase(oo->pool.begin() +
									   static_cast<ptrdiff_t>(worst));
				}
				if(take) {
					OnlineOptions::Entry e;
					e.sig = sig;
					e.score = best.score;
					e.worker = cfg.worker_id;
					e.run.score = best.score;
					e.run.steps = best.flat;   // le mineur consomme `steps`
					oo->pool.push_back(std::move(e));
					++oo->kept;
					// Plafond global : la pire ligne du corpus, tous workers
					// confondus, sort.
					while(oo->pool.size() > oo->max_pool) {
						size_t w = 0;
						for(size_t i = 1; i < oo->pool.size(); ++i)
							if(oo->pool[i].score < oo->pool[w].score)
								w = i;
						oo->pool.erase(oo->pool.begin() +
									   static_cast<ptrdiff_t>(w));
					}
				}
			}
		}
		// 2. RACHAT du catalogue. Le shared_ptr garde vivant celui qu'on quitte
		// tant qu'un autre worker le tient encore.
		if(oo->gen != options_gen) {
			options_hold = oo->cat;
			options_gen = oo->gen;
			cfg.options = options_hold.get();
		}
		// 3. RECLAMATION du tour de minage. Un seul mineur a la fois ; le
		// premier worker a franchir l'echeance le prend.
		const size_t lines = oo->pool.size() + (oo->seed ? oo->seed->size() : 0);
		if(!oo->mining && now >= oo->next && lines >= oo->min_lines) {
			oo->mining = true;
			mine = true;
			m_max = oo->max_options;
			m_sup = oo->support;
			m_len = oo->max_len;
			m_win = oo->window;
			m_ctx = oo->ctx_tol;
			corpus.reserve(lines);
			if(oo->seed)
				for(const NrpaRun& r : *oo->seed)
					corpus.push_back(r);
			for(const auto& e : oo->pool)
				corpus.push_back(e.run);
		}
	}
	if(!mine)
		return;
	// Le minage court VERROU RELACHE : les quinze autres workers continuent de
	// tirer pendant ce temps. Il court aussi dans le budget du run — sa duree
	// est mesuree, et c'est elle qui decide s'il a sa place en ligne.
	const auto t0 = std::chrono::steady_clock::now();
	OptionCatalog cat = MineOptionCatalog(corpus, m_max, m_sup, m_len, m_win,
										  m_ctx);
	const double ms = std::chrono::duration<double, std::milli>(
						  std::chrono::steady_clock::now() - t0).count();
	{
		std::lock_guard<std::mutex> lock(oo->mu);
		oo->mining = false;
		oo->next = std::chrono::steady_clock::now() +
				   std::chrono::milliseconds(
					   static_cast<long long>(oo->period_ms));
		++oo->rounds;
		oo->mine_ms_total += ms;
		oo->mine_ms_max = (std::max)(oo->mine_ms_max, ms);
		oo->last_lines = corpus.size();
		if(cat.Size()) {
			size_t sum = 0, mx = 0;
			for(const auto& s : cat.seqs) {
				sum += s.size();
				mx = (std::max)(mx, s.size());
			}
			oo->last_size = cat.Size();
			oo->last_maxlen = mx;
			oo->last_avglen = double(sum) / double(cat.Size());
			oo->last_flat = cat.model_flat;
			oo->last_opt = cat.model_opt;
			oo->cat = std::make_shared<const OptionCatalog>(std::move(cat));
			++oo->gen;
		}
		// Un tour qui ne retient RIEN laisse le catalogue precedent en place :
		// on ne desarme jamais un worker sur un corpus momentanement pauvre.
		if(oo->gen != options_gen) {
			options_hold = oo->cat;
			options_gen = oo->gen;
			cfg.options = options_hold.get();
		}
	}
}

double Search::NrpaTop(Policy& pol, NrpaRun& best, uint64_t& rng) {
	best.score = -1;
	best.steps.clear();
	// Redemarrage ensemence par la meilleure sequence GLOBALE : l'adaptation
	// tire d'emblee vers la meilleure ligne connue de TOUS les workers, au
	// lieu de reapprendre les memes sous-lignes a chaque redemarrage (mesure :
	// 6/8 avec les memes manquants, run apres run).
	if(cfg.nrpa_shared) {
		std::lock_guard<std::mutex> lock(cfg.nrpa_shared->mu);
		if(cfg.nrpa_shared->best.score > 0)
			best = cfg.nrpa_shared->best;
	}
	uint32_t stagnant = 0, repeats = 0;
	for(uint32_t i = 0; i < cfg.nrpa_iters; ++i) {
		if(BudgetExhausted() ||
		   (!cfg.anytime && solutions.size() >= cfg.max_solutions))
			break;
		// FRONTIERE SURE du minage EN LIGNE : aucun tirage n'est en vol dans ce
		// worker, donc aucun `active_macro` ne pointe dans le catalogue — il
		// peut etre echange ici et nulle part ailleurs.
		OnlineOptionsTick(best);
		NrpaRun child;
		Nrpa(cfg.nrpa_level - 1, pol, child, rng);
		if(child.score > best.score) {
			best = std::move(child);
			stagnant = 0;
			repeats = 0;
		} else if(cfg.nrpa_lr && MatScore(child.score) == MatScore(best.score)) {
			// Repetitions limitees, au niveau superieur aussi : un redemarrage
			// qui re-trouve la meme ligne a fini d'apprendre.
			if(++repeats >= cfg.nrpa_lr)
				break;
		} else if(++stagnant >= 8) {
			break;
		}
		// Echange avec les autres workers — mutex basse frequence : une prise
		// par iteration de niveau superieur (iters^(niveau-1) tirages), pas
		// par tirage. Publier une amelioration ; adopter la meilleure globale
		// seulement en cas de stagnation, pour ne pas ecraser la diversite.
		if(cfg.nrpa_shared) {
			std::lock_guard<std::mutex> lock(cfg.nrpa_shared->mu);
			if(best.score > cfg.nrpa_shared->best.score) {
				cfg.nrpa_shared->best = best;
			} else if(cfg.nrpa_shared->best.score > best.score && stagnant >= 4) {
				best = cfg.nrpa_shared->best;
				stagnant = 0;
			}
		}
		if(best.score >= 0) {
			Adapt(pol, best);
			++stats.nrpa_adapts;
		}
		// HINDSIGHT (chantier 2) : le MEME gradient NRPA, applique aux lignes
		// qui ont atteint un AUTRE but que celui qu'on cherche. C'est tout le
		// mecanisme de HER — l'information est deja la, `Adapt` n'en renforce
		// qu'un millionieme (la seule meilleure sequence), et le reste part a
		// la poubelle a chaque tirage.
		//
		// A `cfg.hindsight` fraction d'alpha : ces lignes ne sont PAS des
		// solutions du probleme pose, seulement des demonstrations de « comment
		// payer une invocation a k materiaux ». Les adapter a plein regime
		// ferait converger la politique vers la Fusion la moins chere, c'est-a-
		// dire vers la panne qu'on corrige.
		if(cfg.hindsight > 0.0f && !hindsight.empty())
			for(const auto& [code, g] : hindsight) {
				(void)code;
				AdaptRun(pol, &ctx_weights, g.run,
						 cfg.nrpa_alpha * cfg.hindsight, cfg.nrpa_bias_known,
						 cfg.hint_bias, cfg.ctx_shrink, cfg.nrpa_temp,
						 cfg.ctx_max, cfg.assign_bias);
				++stats.hindsight_adapts;
			}
	}
	return best.score;
}

void Search::RunNrpa(const BoardKey& t, const std::vector<PlanStep>& p,
					 uint64_t seed) {
	prof::Scope ps(prof::kSearch);
	target = t;
	plan = &p;
	plan_index.clear();
	for(size_t i = 0; i < p.size(); ++i)
		if(p[i].edge)
			plan_index.emplace(p[i].edge, i);
	start = std::chrono::steady_clock::now();
	solutions.clear();
	path.clear();
	novelty.Clear();
	uint64_t rng = seed ? seed : 1;
	// BANDIT DE TETE (--qhat) : la fenetre glissante s'alloue ici, une fois par
	// recherche, et le DENOMINATEUR de la recompense se fixe ici aussi. C'est
	// une constante du PROBLEME (le materiel du board cible) et non le meilleur
	// score courant : normaliser par lui ferait bouger l'echelle des entrees
	// deja dans la fenetre, et Q^ comparerait des recompenses incomparables.
	// Le plancher de 40 est le cimetiere (`fodder` dans Heuristic), qui n'a pas
	// de maximum structurel ; la recompense est plafonnee a 1 de toute facon.
	if(cfg.qhat_depth) {
		if(!perm_win.Ready())
			perm_win.Init(cfg.qhat_window);
		qhat_scale = double(target.codes.size()) * 100.0 +
					 double(target.entries.size()) * 10.0 +
					 double(target.mzone_count) * 3.0 + 40.0 +
					 double(cfg.resolve_weight) * double(resolve_total);
	}

	arena.Push();     // point de reprise : la racine
	// Redemarrages successifs contre la convergence prematuree — mais la
	// politique N'EST PLUS remise a zero : elle est attenuee (persistance
	// partielle). Les sous-lignes apprises survivent au redemarrage, la
	// diversite revient par l'echantillonnage. A 0, comportement d'avant.
	// La politique initiale (cfg.nrpa_init) ensemence le premier redemarrage :
	// le finisseur repart de ce que la phase tirages a appris.
	Policy pol;
	if(cfg.nrpa_init)
		pol = *cfg.nrpa_init;
	// Rejeu d'ADAPTATION du corpus (chantier 5bis) : avant le premier tirage, la
	// politique subit le gradient NRPA des lignes deja resolues. Meme point
	// d'injection que le prior par poids ; ce qui change est la FORME du signal
	// (discriminatif, cf. LiftPolicyRun). L'adaptation est auto-limitante : une
	// fois p(choisi) ~ 1 a une etape, la mise a jour y vaut alpha(1-p) ~ 0 —
	// les passes saturent au lieu d'exploser.
	if(cfg.nrpa_adapt_runs && cfg.nrpa_adapt_passes)
		AdaptCorpus(pol, &ctx_weights, *cfg.nrpa_adapt_runs,
					cfg.nrpa_adapt_passes, cfg.nrpa_alpha, cfg.nrpa_bias_known,
					cfg.hint_bias, cfg.ctx_shrink, cfg.nrpa_temp);
	while(!BudgetExhausted() &&
		  (cfg.anytime || solutions.size() < cfg.max_solutions)) {
		NrpaRun best;
		NrpaTop(pol, best, rng);
		// La politique de fin de redemarrage est la plus riche : c'est elle
		// qu'on exporte (avant attenuation) pour le finisseur LTS.
		final_policy = pol;
		if(cfg.nrpa_restart_keep <= 0.0f) {
			pol.clear();
			ctx_weights.clear();
		} else {
			for(auto it = pol.begin(); it != pol.end();) {
				it->second *= cfg.nrpa_restart_keep;
				// Purge des poids negligeables : la table reste bornee.
				if(it->second > -0.01f && it->second < 0.01f)
					it = pol.erase(it);
				else
					++it;
			}
			// Le niveau contextuel suit la MEME attenuation. Le compteur, lui,
			// survit : c'est l'evidence accumulee dans ce contexte, pas une
			// magnitude apprise — un redemarrage n'invalide pas le fait que
			// cette case ait ete visitee.
			for(auto it = ctx_weights.begin(); it != ctx_weights.end();) {
				it->second.w *= cfg.nrpa_restart_keep;
				if(it->second.w > -0.01f && it->second.w < 0.01f)
					it = ctx_weights.erase(it);
				else
					++it;
			}
		}
	}
	arena.Pop();

	stats.ms = std::chrono::duration<double, std::milli>(
				   std::chrono::steady_clock::now() - start).count();
	stats.hit_node_limit = stats.nodes >= cfg.max_nodes;
	stats.exhausted = false;
	stats.novelty_atoms = novelty.Size();
	// La vie du niveau contextuel, relevee LA OU IL TRAVAILLE (piege 52) : une
	// table au plafond n'apprend plus rien de neuf, une table minuscule dit que
	// le conditionnement ne distingue rien.
	stats.ctx_entries = ctx_weights.size();
	stats.ctx_capped = cfg.ctx_max && ctx_weights.size() >= cfg.ctx_max;
	// La vie du BANDIT : taille de l'arbre de tete, vocabulaire de la fenetre,
	// et MEMOIRE mesuree — le cout de ce mecanisme est en memoire et il est paye
	// par worker. Sans ce chiffre, W se reglerait a l'aveugle.
	stats.qhat_nodes = bandit.size();
	stats.qhat_codes = perm_win.Codes();
	stats.qhat_bytes = perm_win.Bytes();
}

void Search::RunLevin(const BoardKey& t, const std::vector<PlanStep>& p,
					  const NrpaPolicy& pol) {
	prof::Scope ps(prof::kSearch);
	target = t;
	plan = &p;
	plan_index.clear();
	for(size_t i = 0; i < p.size(); ++i)
		if(p[i].edge)
			plan_index.emplace(p[i].edge, i);
	start = std::chrono::steady_clock::now();
	solutions.clear();
	path.clear();
	tt.clear();

	// Un noeud = un prompt a CHOIX MULTIPLE ; les coups forces entre deux
	// noeuds sont rejoues, pas stockes. Le chemin d'un noeud se reconstruit en
	// remontant les parents — c'est ce qui permet le best-first sur un moteur
	// qui ne sait que rejouer.
	struct LNode {
		int32_t parent;
		uint32_t depth;                  // decisions reelles (choix multiples)
		float logpi;                     // log du produit des probabilites
		std::vector<uint8_t> response;   // reponse appliquee depuis le parent
		// --- sqrt-LTS (cfg.levin_reroot) ---
		// log de pi(n | n_k) : la probabilite RELATIVE a l'ancetre-indice le
		// plus proche. C'est elle qui repart de zero a chaque indice, et c'est
		// tout le mecanisme.
		float seg_logpi = 0.0f;
		// lambda/pi(n ; n_k) = somme des 1/pi(n_bar | n_k) sur le segment.
		double lam = 1.0;
		// Cartes du board cible posees chez le PARENT : un indice tombe quand
		// le noeud n'en a pas le meme compte (le board a change de sous-but).
		uint16_t placed_parent = 0;
		// --- sqrt-LTS-H (cfg.reroot_h) ---
		// `hv` = c(n) = min sur les ancetres de (1/w_t) * c^r_{n_t}(n), la
		// valeur qui ordonne la file. `hu` = le terme (1/w_t) * (1/pi(n|n_t))
		// de l'ancetre qui realise ce min : c'est lui qui permet d'etendre la
		// somme d'un cran sans reparcourir le chemin.
		double hv = 0.0, hu = 0.0;
		// --- arete MACRO (cfg.finisher_options) ---
		// Indice de la macro dans cfg.options. UINT32_MAX = arete ordinaire.
		// `response` reste la PREMIERE reponse de la macro ; les suivantes se
		// retrouvent au rejeu, cle par cle, aux prompts intermediaires — elles
		// ne sont pas stockees (elles ne sont pas des choix de l'arbre).
		uint32_t macro = UINT32_MAX;
		// Decisions REELLES sur le chemin. `depth` compte les ARETES — c'est le
		// d(n) de la borne de Levin, et c'est bien lui qui doit tomber quand une
		// macro compresse k decisions en une. Mais le PLAFOND de decisions est
		// une borne sur le jeu, pas sur l'arbre : une arete de longueur 8 avance
		// de 8 decisions, et le laisser compter pour 1 rendrait le plafond huit
		// fois plus lache sans que rien ne le dise. Les deux sont donc separes.
		uint32_t rdepth = 0;
	};
	std::vector<LNode> nodes;
	nodes.push_back({ -1, 0, 0.0f, {}, 0.0f, 1.0, 0xffffu,
					  std::numeric_limits<double>::infinity(),
					  std::numeric_limits<double>::infinity(), UINT32_MAX, 0u });
	// h(racine) de l'Eq. 7 : l'echelle qui rend le rerooter doux invariant par
	// changement d'unite de l'heuristique. C'est h A LA RACINE DE LA RECHERCHE
	// COURANTE, pas une constante du probleme — la session 8 prenait
	// |cible| + Sigma resolve_min, ce qui est h au depart du DUEL. Dans le
	// finisseur la racine est un etat de recul deja a 7/8 cartes : h y vaut ~1,
	// et le cadran alpha etait donc parcouru a une echelle huit fois trop
	// grande. Evalue au developpement du noeud 0, qui precede toujours tout
	// usage (il est enfile a cout nul et sort en premier). Plancher a 1.
	double h_root = 1.0;
	// Cout de Levin : d(n)/pi(n), en log pour la stabilite. Le noeud de
	// moindre cout est developpe en premier — c'est la garantie du papier :
	// nombre d'expansions borne par la probabilite de la solution sous la
	// politique.
	using QE = std::pair<double, uint32_t>;
	// Departage des ex aequo (cfg.lifo_ties) : std::greater sur la paire
	// extrayait le plus PETIT indice — le noeud le plus ancien, le plus loin
	// de la pile de plongee. En LIFO, l'ex aequo extrait est le dernier
	// enfile : presque toujours un enfant du noeud qu'on vient de developper,
	// dont le rejeu est UNE reponse depuis le sommet de pile.
	struct QCmp {
		bool lifo;
		bool operator()(const QE& a, const QE& b) const {
			if(a.first != b.first)
				return a.first > b.first;
			return lifo ? a.second < b.second : a.second > b.second;
		}
	};
	std::priority_queue<QE, std::vector<QE>, QCmp> pq(QCmp{ cfg.lifo_ties });
	pq.push({ 0.0, 0 });

	std::vector<uint32_t> chain;
	std::vector<double> logit;
	// Macros applicables au noeud developpe : (indice de macro, indice du choix
	// qui porte sa premiere cle). Tampon reutilise entre expansions.
	std::vector<std::pair<uint32_t, uint32_t>> lapp;
	uint32_t actions = 0, turns = 0, summons = 0;
	uint64_t resolved = 0;
	enum class Adv { Branch, Dead, Goal };
	// Total des resolutions exigees (--resolve) : la part "resolutions
	// manquantes" de l'heuristique PHS*.
	uint32_t resolve_target = 0;
	for(const ResolveReq& req : cfg.resolve_min)
		resolve_target += req.min_count;

	// Avance jusqu'au prochain prompt a choix multiple en jouant les coups
	// forces (l'adversaire passe, les selections a candidat unique — une
	// decision forcee coute zero). `check` : controles de but/tour/garde,
	// actifs seulement sur le segment NOUVEAU (apres la derniere reponse du
	// chemin) — les segments anterieurs ont ete controles au developpement de
	// leurs noeuds, y refaire le board serait payer deux requetes de zone par
	// prompt de rejeu pour rien.
	auto advance = [&](bool check) -> Adv {
		for(;;) {
			Step st = StepToPrompt();
			actions += actions_this_step;
			turns += turns_this_step;
			if(st == Step::Rejected) { ++stats.dead_ends; return Adv::Dead; }
			if(st == Step::Ended)    { ++stats.terminals; return Adv::Dead; }
			if(!SummonsOk(summons) || material_violation) {
				++stats.constraint_cuts;
				return Adv::Dead;
			}
			summons += static_cast<uint32_t>(summons_this_step.size());
			resolved += resolved_this_step;
			if(check) {
				ComputeBoardKeyInto(duel, static_cast<uint8_t>(cfg.target_player),
									board_scratch);
				if(GoalCheck(board_scratch, static_cast<uint32_t>(path.size()),
							 actions, resolved)) {
					// Apres-but (opt-in) : sous anytime la ligne CONTINUE —
					// GoalCheck a deja enregistre la solution, la suite peut
					// la rendre moins chere (recuperations, piege 35).
					if(!(cfg.anytime && cfg.finisher_post_goal))
						return Adv::Goal;
				}
				if(turns >= 2) { ++stats.turn_cuts; return Adv::Dead; }
				if(GuardCut(board_scratch, summons))
					return Adv::Dead;
			} else if(turns >= 2) {
				return Adv::Dead;
			}
			if(!FillChoices(ro_choices)) {
				++stats.dead_ends;
				return Adv::Dead;
			}
			if(ro_choices.size() > 1)
				return Adv::Branch;
			duel.SetResponse(ro_choices[0].response);
			path.push_back(ro_choices[0].response);
		}
	};

	// SUITE d'une ARETE MACRO (chantier 3). Sa PREMIERE reponse vient d'etre
	// appliquee par l'appelant, comme pour une arete ordinaire ; les cles
	// suivantes se jouent en avancant jusqu'a chaque prompt a choix multiple et
	// en y retrouvant la cle. Une cle absente AVORTE l'arete — elle devient
	// morte, exactement comme une macro qui casse au rollout. Rend false quand
	// l'arete est morte (l'appelant s'arrete), true quand la macro est jouee
	// jusqu'au bout (l'appelant reprend son flux : c'est LUI qui fera l'advance
	// suivant, comme pour une arete ordinaire).
	//
	// `check` a le meme sens que pour advance : vrai seulement sur le segment
	// NOUVEAU de la chaine. Les compteurs ne s'incrementent que la — sur les
	// prefixes, la meme arete se rejoue des milliers de fois et un compteur qui
	// l'y compterait ne mesurerait que le taux de rejeu.
	auto play_rest = [&](uint32_t n, bool check) -> bool {
		if(nodes[n].macro == UINT32_MAX)
			return true;
		const std::vector<uint64_t>& seq = cfg.options->seqs[nodes[n].macro];
		for(size_t k = 1; k < seq.size(); ++k) {
			if(advance(check) != Adv::Branch)
				return false;
			size_t hit = SIZE_MAX;
			for(size_t i = 0; i < ro_choices.size(); ++i)
				if(ro_choices[i].plan_key == seq[k]) {
					hit = i;
					break;
				}
			if(hit == SIZE_MAX) {
				if(check)
					++stats.macro_aborted;
				return false;
			}
			if(check)
				++stats.macro_absorbed;
			duel.SetResponse(ro_choices[hit].response);
			path.push_back(ro_choices[hit].response);
		}
		return true;
	};

	// Pile de plongee : le niveau d'arene racine+1+i est l'etat du noeud
	// dive[i].node A SON PROMPT. Sous une politique affutee, le best-first se
	// comporte quasi en profondeur d'abord : le prochain noeud extrait est
	// presque toujours un enfant ou un frere d'un noeud de la pile — son rejeu
	// se reduit alors a depiler jusqu'au parent (Pop restaure, jamais
	// Discard : lui ne verse pas les pages sales du fils dans le parent) et
	// jouer UNE reponse, au lieu de rejouer toute la chaine depuis la racine.
	struct DiveLevel {
		uint32_t node;
		size_t path_len;
		uint32_t actions, turns, summons;
		uint64_t resolved;
	};
	std::vector<DiveLevel> dive;

	arena.Push();
	while(!pq.empty()) {
		if(BudgetExhausted()) {
			stats.hit_time_limit = true;
			break;
		}
		if(!cfg.anytime && solutions.size() >= cfg.max_solutions)
			break;
		// Garde-fou memoire : chaque enfant enfile porte sa reponse ; au-dela
		// de quelques millions de noeuds, le budget temps est de toute facon
		// consomme par les rejeux.
		if(stats.nodes >= cfg.max_nodes || nodes.size() > 4000000) {
			stats.hit_node_limit = true;
			if(nodes.size() > 4000000)
				stats.hit_memory_limit = true;
			break;
		}
		const uint32_t idx = pq.top().second;
		pq.pop();
		const int32_t parent = nodes[idx].parent;

		// Chaine d'ancetres de idx (racine exclue, idx compris), de la racine
		// vers idx.
		chain.clear();
		for(int32_t i = static_cast<int32_t>(idx); i > 0; i = nodes[i].parent)
			chain.push_back(static_cast<uint32_t>(i));
		std::reverse(chain.begin(), chain.end());
		// Aretes DEJA DEVELOPPEES de la chaine (tout sauf celle d'idx) : le
		// denominateur du taux de rejeu — une pile qui absorbe tout en rejoue 0.
		if(!chain.empty())
			stats.replay_chain += chain.size() - 1;

		// L'ANCETRE le plus profond present sur la pile de plongee — pas
		// seulement le parent direct. Le profil (§9.18) a mesure ~80 Process
		// par expansion : tout saut de la file vers un autre sous-arbre
		// rejouait la chaine ENTIERE depuis la racine, alors que la pile
		// detenait le prefixe commun. Depiler jusqu'a l'ancetre partage et ne
		// rejouer que le SUFFIXE est semantiquement neutre — memes reponses,
		// memes controles, seul le point de depart du rejeu change. Controle :
		// l'etalon 0 (recul 0 doit rendre 42 exp., b=0, EPUISE).
		int64_t at = -1;
		size_t ci = 0;
		for(size_t j = dive.size(); j-- > 0;) {
			const uint32_t dn = dive[j].node;
			if(dn == 0) {   // la racine est l'ancetre de tout noeud
				at = static_cast<int64_t>(j);
				ci = 0;
				break;
			}
			bool anc = false;
			for(size_t k = chain.size(); k-- > 0;)
				if(chain[k] == dn) {
					anc = true;
					ci = k + 1;   // premier coup a rejouer : l'enfant de dn
					break;
				}
			if(anc) {
				at = static_cast<int64_t>(j);
				break;
			}
		}

		bool dead = false;
		if(parent >= 0 && at < 0) {
			// Pile vide (ou sans ancetre commun) : rejeu depuis la racine,
			// les coups forces se re-derivent.
			++stats.dive_misses;
			if(cfg.merged_pop) {
				arena.PopToAndRestore(dive.size());
				dive.clear();
			} else {
				while(!dive.empty()) {
					arena.Pop();
					dive.pop_back();
				}
				arena.Restore();
			}
			path.clear();
			actions = 0;
			turns = cfg.initial_turns;
			summons = cfg.initial_summons;
			resolved = cfg.initial_resolved;
			ci = 0;
			for(;;) {
				Adv a = advance(ci == chain.size());
				if(a != Adv::Branch) {
					dead = true;   // mort ou but : pas d'enfants
					break;
				}
				if(ci == chain.size())
					break;
				// Pile complete : l'etat courant est le prompt de chain[ci-1]
				// (la racine pour ci = 0), un noeud deja developpe — donc un
				// point de branchement possible des extractions futures.
				if(cfg.dive_full) {
					arena.Push();
					dive.push_back({ ci == 0 ? 0u : chain[ci - 1], path.size(),
									 actions, turns, summons, resolved });
				}
				const std::vector<uint8_t>& r = nodes[chain[ci]].response;
				duel.SetResponse(r);
				path.push_back(r);
				++ci;
				if(ci < chain.size())
					++stats.replay_decisions;
				// Arete macro : ses cles suivantes se jouent maintenant, avant
				// que la boucle ne reprenne son advance.
				if(!play_rest(chain[ci - 1], ci == chain.size())) {
					dead = true;
					break;
				}
			}
		} else {
			// Depiler jusqu'a l'ancetre partage (chaque Pop restaure et
			// fusionne ses pages sales), restaurer SON etat — il est A SON
			// PROMPT — puis rejouer le suffixe de chaine : au plus court, la
			// seule reponse de idx (enfant ou frere, l'ancien chemin rapide).
			if(cfg.merged_pop) {
				arena.PopToAndRestore(dive.size() -
									  static_cast<size_t>(at + 1));
				dive.resize(static_cast<size_t>(at + 1));
			} else {
				while(static_cast<int64_t>(dive.size()) > at + 1) {
					arena.Pop();
					dive.pop_back();
				}
				arena.Restore();
			}
			if(at >= 0) {
				const DiveLevel& L = dive[static_cast<size_t>(at)];
				path.resize(L.path_len);
				actions = L.actions;
				turns = L.turns;
				summons = L.summons;
				resolved = L.resolved;
			} else {
				path.clear();
				actions = 0;
				turns = cfg.initial_turns;
				summons = cfg.initial_summons;
				resolved = cfg.initial_resolved;
			}
			if(idx == 0) {
				dead = advance(true) != Adv::Branch;
			} else {
				for(;;) {
					duel.SetResponse(nodes[chain[ci]].response);
					path.push_back(nodes[chain[ci]].response);
					++ci;
					if(ci < chain.size())
						++stats.replay_decisions;
					if(!play_rest(chain[ci - 1], ci == chain.size())) {
						dead = true;
						break;
					}
					Adv a = advance(ci == chain.size());
					if(a != Adv::Branch) {
						dead = true;
						break;
					}
					if(ci == chain.size())
						break;
					// Pile complete : on vient d'arriver au prompt de
					// chain[ci-1], deja developpe — l'empiler pour que les
					// sauts futurs atterrissent ici au lieu de la racine.
					if(cfg.dive_full) {
						arena.Push();
						dive.push_back({ chain[ci - 1], path.size(),
										 actions, turns, summons, resolved });
					}
				}
			}
		}
		if(dead)
			continue;

		// Transposition : en best-first, la premiere visite est celle de
		// moindre cout — les suivantes sont des doublons.
		const uint64_t key = Digest();
		if(tt.count(key)) {
			++stats.transpositions;
			continue;
		}
		tt.emplace(key, 1u);
		++stats.nodes;
		prof::Count(prof::kDecisions);
		const uint32_t ndepth = nodes[idx].depth;
		const uint32_t nrdepth = nodes[idx].rdepth;
		const float nlogpi = nodes[idx].logpi;
		if(nrdepth >= cfg.max_decisions) {
			++stats.edges_skipped;
			continue;
		}
		// Le noeud developpe rejoint la pile de plongee : ses enfants et ses
		// freres se rejoueront en une restauration au lieu d'un rejeu complet.
		arena.Push();
		dive.push_back({ idx, path.size(), actions, turns, summons, resolved });

		// PHS* : distance au but du noeud developpe (cartes cibles manquantes
		// + resolutions manquantes), heritee par ses enfants dans le cout.
		// `board_scratch` est le board du noeud (dernier advance en mode
		// controle). A 0, Levin pur.
		float hgoal = 0;
		uint32_t got = 0;
		if(cfg.levin_h > 0 || cfg.levin_reroot || cfg.reroot_h > 0)
			got = CommonCodes(board_scratch.codes, target.codes);
		if(cfg.levin_h > 0 || cfg.reroot_h > 0) {
			hgoal = static_cast<float>(target.codes.size() - got) +
					static_cast<float>(resolve_target - ResolveProgress(resolved));
		}
		// GRAPHE DE RECETTES : la distance qui DECROIT.
		//
		// `h` plat compte les cartes cibles manquantes — il ne bouge pas tant
		// qu'aucune n'est posee, c'est-a-dire sur ~90 % de la ligne. La distance
		// de recettes compte les INVOCATIONS qui restent a faire, materiaux
		// intermediaires compris : poser Leo Dancer au cimetiere ne pose aucune
		// carte cible, mais fait tomber la distance a Liger Dancer de 2 a 1.
		//
		// Toujours EVALUEE quand le graphe existe, meme si `recipe_h` vaut 0 :
		// c'est le mode qui chiffre ce que le graphe saurait dire AVANT de le
		// laisser decider (piege 40 — instrumenter avant de calibrer).
		if(cfg.recipes) {
			const float rh = RecipeDistance(board_scratch, resolved);
			stats.recipe_h_sum += rh;
			++stats.recipe_h_count;
			hgoal += cfg.recipe_h * rh;
		}
		// LANDMARKS APPRIS (chantier 18) : le MEME point d'entree que le graphe
		// de recettes, et la meme discipline — toujours EVALUE des que le graphe
		// existe (le compteur dit ce qu'il saurait dire), il ne PESE que si son
		// poids est non nul.
		if(cfg.landmarks && !cfg.landmarks->Empty()) {
			const uint32_t lrem = LandmarkRemaining(board_scratch);
			hgoal += cfg.landmark_h * static_cast<float>(lrem);
		}
		// Eq. 7 : l'echelle est h a la racine de CETTE recherche (C3).
		if(idx == 0 && cfg.reroot_h > 0) {
			h_root = (std::max)(1.0, static_cast<double>(hgoal));
			stats.h_root = h_root;
		}
		// sqrt-LTS-H : le poids de re-enracinement de CE noeud. La racine vaut
		// 1 par convention de l'article ; ailleurs, plus le noeud est loin du
		// but, plus se re-enraciner sur lui coute cher.
		double inv_w = 1.0;
		if(cfg.reroot_h > 0 && idx != 0)
			inv_w = std::exp(static_cast<double>(cfg.reroot_h) * hgoal / h_root);
		// sqrt-LTS : ce noeud est-il un INDICE ? La racine en est toujours un
		// (sans quoi les noeuds situes avant le premier indice n'auraient aucun
		// ancetre re-enracineur). Au re-enracinement, le segment repart : la
		// probabilite relative revient a 1, donc lambda/pi revient a 1.
		double base_lam = nodes[idx].lam;
		float base_seg = nodes[idx].seg_logpi;
		if(cfg.levin_reroot &&
		   (idx == 0 || got != nodes[idx].placed_parent)) {
			base_lam = 1.0;
			base_seg = 0.0f;
			++stats.reroots;
		}

		// Softmax de la politique sur les choix — les MEMES logits que les
		// tirages NRPA (poids plan_key + biais repertoire + biais indices),
		// pour que la politique apprise garde exactement son sens.
		const size_t nc = ro_choices.size();
		logit.resize(nc);
		double mx = -1e300;
		for(size_t i = 0; i < nc; ++i) {
			const Choice& c = ro_choices[i];
			const bool known = plan_index.count(c.plan_key) != 0 && !c.phase;
			const bool hinted = c.card && !cfg.hint_cards.empty() &&
								std::find(cfg.hint_cards.begin(),
										  cfg.hint_cards.end(),
										  c.card) != cfg.hint_cards.end();
			auto pit = pol.find(c.plan_key);
			const double w = (pit == pol.end()) ? 0.0 : pit->second;
			logit[i] = w + (known ? cfg.nrpa_bias_known : 0.0f) +
					   (hinted ? cfg.hint_bias : 0.0f);
			mx = (std::max)(mx, logit[i]);
		}
		// ARETES MACRO (chantier 3). Memes gardes qu'au rollout : UNE macro par
		// premiere cle (la mieux classee parmi les compatibles), AUCUN biais
		// herite (le poids est celui que la politique a appris sur son id). La
		// fenetre positionnelle n'a pas de sens ici — le finisseur ne compte
		// pas de decisions depuis un depart de tirage — et elle est refutee
		// (9.19 (g)) : seule la garde SEMANTIQUE s'applique.
		//
		// Le denominateur du softmax grossit donc de `lapp.size()` : les aretes
		// atomiques deviennent MOINS probables. C'est exactement pourquoi le
		// compte d'expansions de l'etalon 0 n'est plus le controle (voir
		// SearchConfig::finisher_options).
		lapp.clear();
		if(cfg.finisher_options && cfg.options) {
			uint16_t mctx = 0;
			if(cfg.options->ctx_tol >= 0)
				mctx = ContextKey(
					CommonCodes(board_scratch.codes, target.codes),
					duel.Count(static_cast<uint8_t>(cfg.target_player),
							   LOCATION_HAND));
			for(size_t i = 0; i < nc; ++i) {
				auto it = cfg.options->by_first.find(ro_choices[i].plan_key);
				if(it == cfg.options->by_first.end())
					continue;
				for(uint32_t mi : it->second) {
					if(!cfg.options->CtxOk(mi, mctx))
						continue;
					lapp.emplace_back(mi, static_cast<uint32_t>(i));
					break;   // une macro par premiere cle
				}
			}
			for(const auto& mp : lapp) {
				auto pit = pol.find(cfg.options->ids[mp.first]);
				logit.push_back((pit == pol.end()) ? 0.0 : pit->second);
				mx = (std::max)(mx, logit.back());
				++stats.macro_taken;
			}
		}
		double sum = 0;
		for(double& x : logit) {
			x = std::exp(x - mx);
			sum += x;
		}
		for(size_t i = 0; i < logit.size(); ++i) {
			LNode child;
			child.parent = static_cast<int32_t>(idx);
			child.depth = ndepth + 1;
			child.logpi = nlogpi + static_cast<float>(std::log(
				(std::max)(logit[i] / sum, 1e-30)));
			if(i < nc) {
				child.response = ro_choices[i].response;
				child.rdepth = nrdepth + 1;
			} else {
				// Arete macro : sa reponse est le premier coup de la macro ; sa
				// PROFONDEUR D'ARBRE reste 1 — c'est toute la compression — mais
				// elle avance de |macro| decisions reelles.
				const auto& mp = lapp[i - nc];
				child.response = ro_choices[mp.second].response;
				child.macro = mp.first;
				child.rdepth = nrdepth +
					static_cast<uint32_t>(cfg.options->seqs[mp.first].size());
			}
			double lcost;
			if(cfg.reroot_h > 0) {
				// Eq. 3 de arXiv:2605.30664, tenue en O(1). Deux candidats
				// seulement : PROLONGER l'ancetre qui minimisait deja le cout
				// chez le parent, ou SE RE-ENRACINER sur le parent lui-meme.
				// Le second est toujours disponible et vaut (1/w_parent)/pi :
				// il BORNE le cout, ce qui rend le mecanisme numeriquement
				// stable la ou d/pi deborde. L'ecart avec le min exact sur tous
				// les ancetres est donc majore par ce terme, et unilateral (on
				// ne sous-estime jamais).
				const double pi_c = (std::max)(logit[i] / sum, 1e-30);
				const double ext_u = nodes[idx].hu / pi_c;
				const double ext_v = nodes[idx].hv + ext_u;
				const double new_u = inv_w / pi_c;
				// DEBORDEMENT. Quelques niveaux a pi_c ~ 1e-30 suffisent a
				// envoyer hu a l'infini ; `ext_v <= new_u` devient alors faux
				// pour une raison purement ARITHMETIQUE, la branche
				// « re-enracinement » est prise, et stats.reroots — dont le
				// commentaire dit qu'il est l'instrument de « a zero, le
				// mecanisme est inerte » — devient non nul exactement quand le
				// calcul a casse. On compte les deux separement (4.12).
				//
				// MAIS la RACINE porte hu = hv = +inf PAR CONVENTION : elle n'a
				// aucun ancetre a prolonger, donc ses enfants DOIVENT se
				// re-enraciner, et c'est correct. Les compter comme un
				// debordement etait un FAUX POSITIF, et il allumait le drapeau
				// sur toutes les lignes de toutes les racines — l'instrument
				// accusait le mecanisme de ce qui est sa definition. Le vrai
				// debordement est celui qui part d'un parent FINI.
				const bool parent_finite = std::isfinite(nodes[idx].hu) &&
										   std::isfinite(nodes[idx].hv);
				const bool overflowed =
					parent_finite &&
					(!std::isfinite(ext_v) || !std::isfinite(ext_u));
				if(overflowed)
					++stats.levin_overflow;
				if(ext_v <= new_u) {
					child.hv = ext_v;
					child.hu = ext_u;
				} else {
					child.hv = new_u;
					child.hu = new_u;
					if(overflowed)
						++stats.reroot_by_overflow;
					// Le compteur ne dit plus « un indice est tombe » (avec ce
					// rerooter chaque noeud en est un) mais « le
					// re-enracinement a BATTU la prolongation » : a zero, le
					// mecanisme est inerte et l'A/B ne mesure rien.
					++stats.reroots;
				}
				child.placed_parent = static_cast<uint16_t>(got);
				lcost = std::log((std::max)(child.hv, 1e-300)) +
						static_cast<double>(cfg.levin_h) * hgoal;
			} else if(cfg.levin_reroot) {
				child.seg_logpi = base_seg + static_cast<float>(std::log(
					(std::max)(logit[i] / sum, 1e-30)));
				// lambda/pi(enfant ; n_k) = lambda/pi(parent ; n_k) +
				// 1/pi(enfant | n_k). Le terme ajoute est borne par le cout du
				// SEGMENT, pas par celui de la ligne entiere : c'est ce qui
				// evite le sous-debordement qui rend d/pi inexploitable ici.
				const double inv = std::exp(-static_cast<double>(child.seg_logpi));
				// SATURATION. `seg_logpi` est un float qui accumule des
				// log-probabilites ; passe ~709 en valeur absolue, exp deborde
				// et le garde substitue 1e300. A partir de la, log(lam - 1) est
				// CONSTANT pour toute la descendance et le best-first sqrt-LTS
				// degenere en son departage — l'A/B de --reroot mesurerait alors
				// un mecanisme qui s'est eteint tout seul. Compte (4.12) ; a non
				// nul, le bras est a jeter, pas a interpreter.
				if(!std::isfinite(inv))
					++stats.levin_overflow;
				child.lam = base_lam + (std::isfinite(inv) ? inv : 1e300);
				if(child.lam >= 1e300)
					++stats.lam_saturated;
				child.placed_parent = static_cast<uint16_t>(got);
				// Cout sqrt-LTS (Eq. 14) : lambda/pi - 1, en log pour rester
				// commensurable avec le terme PHS* d'heuristique. Les poids de
				// re-enracinement etant uniformes, leur diviseur est une
				// constante : le best-first y est invariant.
				lcost = std::log((std::max)(child.lam - 1.0, 1e-300)) +
						static_cast<double>(cfg.levin_h) * hgoal;
			} else if(cfg.phs_canonical) {
				// PHS* du papier : (d + h)/pi. h s'ajoute a la profondeur —
				// une carte manquante compte comme une decision de plus, pas
				// comme un facteur e^h.
				lcost = std::log(static_cast<double>(child.depth) + 1.0 +
								 static_cast<double>(cfg.levin_h) * hgoal) -
						child.logpi;
			} else {
				lcost = std::log(static_cast<double>(child.depth) + 1.0) +
						static_cast<double>(cfg.levin_h) * hgoal - child.logpi;
			}
			++stats.levin_children;
			nodes.push_back(std::move(child));
			pq.push({ lcost, static_cast<uint32_t>(nodes.size() - 1) });
		}
	}
	const bool drained = pq.empty();
	while(!dive.empty()) {
		arena.Pop();
		dive.pop_back();
	}
	arena.Pop();

	stats.ms = std::chrono::duration<double, std::milli>(
				   std::chrono::steady_clock::now() - start).count();
	// Epuise = la file s'est videe : l'espace atteignable sous ces bornes a
	// ete ENTIEREMENT enumere — la preuve d'absence du finisseur mono-etat,
	// mais par racine cette fois.
	stats.exhausted = drained && !stats.hit_time_limit && !stats.hit_node_limit;
}

void Search::RunTransplant(const BoardKey& t, const std::vector<PlanStep>& p,
						   uint32_t discrepancies) {
	prof::Scope ps(prof::kSearch);
	target = t;
	plan = &p;
	start = std::chrono::steady_clock::now();
	solutions.clear();
	path.clear();
	tt.clear();
	novelty.Clear();
	cfg_discrepancies = discrepancies;
	plan_index.clear();
	for(size_t i = 0; i < p.size(); ++i)
		if(p[i].edge)
			plan_index.emplace(p[i].edge, i);
	DescendTransplant(0, 0, discrepancies, 0, 0, 0, 0);
	stats.ms = std::chrono::duration<double, std::milli>(
				   std::chrono::steady_clock::now() - start).count();
	stats.hit_node_limit = stats.nodes >= cfg.max_nodes;
	stats.exhausted = !stats.hit_time_limit && !stats.hit_node_limit;
	stats.novelty_atoms = novelty.Size();
}

size_t LiftPlan(Duel& duel, Arena& arena, const Replay& yrp, int target_player,
				size_t stop_after, const EnumOptions& eo,
				std::vector<PlanStep>& out) {
	size_t unknown = 0, ri = 0;
	uint8_t ptype = 0;
	std::vector<uint8_t> payload;
	int player = -1;

	// Applique une reponse et rend l'empreinte de l'etat atteint, prompt
	// compris. 0 signale un rejet par le core (donc aucune correspondance
	// possible) ; la collision avec une empreinte legitime est negligeable.
	auto advance = [&](const std::vector<uint8_t>& resp) -> uint64_t {
		duel.SetResponse(resp);
		uint8_t nt = 0;
		std::vector<uint8_t> np;
		for(;;) {
			int status = duel.Process();
			for(const Message& m : duel.Messages()) {
				if(m.type == MSG_RETRY)
					return 0;
				if(IsPrompt(m.type)) {
					nt = m.type;
					np.assign(m.data, m.data + m.size);
				}
			}
			if(status != OCG_DUEL_STATUS_CONTINUE)
				break;
		}
		return StateDigest(duel, nt, np);
	};

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
			continue;
		if(ri >= yrp.responses.size() || ri >= stop_after)
			break;

		const std::vector<uint8_t>& recorded = yrp.responses[ri];
		if(player == target_player) {
			auto choices = Enumerate(ptype, payload.data(),
									 static_cast<uint32_t>(payload.size()), eo);
			arena.Push();
			uint64_t want = advance(recorded);
			arena.Restore();
			bool found = false;
			for(const Choice& c : choices) {
				uint64_t got = advance(c.response);
				arena.Restore();
				if(want && got == want) {
					// plan_key et non edge : c'est l'identite debarrassee de ce
					// que l'equivalence de board ignore deja (la colonne). Y
					// mettre l'arete rendrait tout choix de zone inappariable.
					out.push_back({ ptype, c.plan_key, c.label });
					found = true;
					break;
				}
			}
			arena.Pop();
			if(!found) {
				++unknown;
				out.push_back({ ptype, 0, "non identifiee" });
			}
		}
		duel.SetResponse(recorded);
		++ri;
	}
	return unknown;
}

// LES FAITS D'UN ETAT (chantier 18). Cinq requetes de zone : c'est cher, et
// c'est sans importance — cette fonction ne tourne que HORS LIGNE, au rejeu du
// corpus de plans resolus (une poignee de lignes de quelques centaines de
// decisions). Le chemin chaud, lui, ne releve QUE les cles de landmark
// (Search::LandmarkRemaining), ce qui est une tout autre facture.
void CollectStateFacts(Duel& duel, uint8_t con,
					   std::unordered_map<uint64_t, uint32_t>& out) {
	out.clear();
	static thread_local std::vector<QueriedCard> rq;
	for(uint32_t loc : { LOCATION_MZONE, LOCATION_SZONE, LOCATION_GRAVE,
						 LOCATION_REMOVED, LOCATION_HAND }) {
		duel.Query(con, loc, QUERY_CODE | QUERY_ALIAS, rq);
		for(const QueriedCard& c : rq)
			if(c.present)
				++out[LandmarkGraph::KeyOf(
					duel.Db().Canonical(c.Code()),
					NormalizeZone(static_cast<uint8_t>(loc)))];
	}
	// LES ZONES DE L'ADVERSAIRE, et ce n'est pas un raffinement.
	//
	// La moitie du but de l'etalon B est un HANDRIP : trois cartes arrachees a
	// la main adverse. Un releve limite au joueur cible ne peut pas l'exprimer —
	// aucun fait de sa propre moitie de terrain ne devient vrai quand
	// l'adversaire perd une carte. Le graphe aurait donc appris a construire le
	// board et serait reste MUET sur exactement la moitie qui manque, tout en
	// ayant l'air de fonctionner. C'est la panne la plus chere du dossier (un
	// diagnostic qui repond a cote de sa propre question), evitee ici avant
	// d'avoir coute un run.
	//
	// Marquees par le bit 0x80 sur la zone : « Ash Blossom au cimetiere
	// ADVERSE » est un fait distinct de « Ash Blossom a mon cimetiere ».
	const uint8_t opp = con ^ 1;
	for(uint32_t loc : { LOCATION_GRAVE, LOCATION_REMOVED, LOCATION_HAND }) {
		duel.Query(opp, loc, QUERY_CODE | QUERY_ALIAS, rq);
		for(const QueriedCard& c : rq)
			if(c.present)
				++out[LandmarkGraph::KeyOf(
					duel.Db().Canonical(c.Code()),
					static_cast<uint8_t>(
						NormalizeZone(static_cast<uint8_t>(loc)) | 0x80))];
	}
}

size_t LiftPolicyRun(Duel& duel, Arena& arena, const Replay& yrp,
					 int target_player, size_t stop_after, const EnumOptions& eo,
					 const std::unordered_map<uint64_t, size_t>& repertoire,
					 const BoardKey& target, NrpaRun& out,
					 RecipeGraph* recipes, uint32_t mcps_depth,
					 LandmarkTrace* landmarks) {
	size_t unknown = 0, ri = 0;
	uint8_t ptype = 0;
	std::vector<uint8_t> payload;
	int player = -1;
	BoardKey here;
	out.score = 0;
	out.steps.clear();
	// Le MEME chemin qu'au tirage : somme melangee des coups joues sur les
	// `mcps_depth` premieres decisions enregistrees de CETTE ligne.
	uint64_t path_ctx = 0;
	uint32_t nsteps = 0;

	// CORPUS -> GRAPHE (revue session 12) : les invocations de la ligne
	// rejouee sont des recettes OBSERVEES — la meme logique que StepToPrompt
	// (MSG_MOVE a REASON_MATERIAL accumule, MSG_SUMMONING/SPSUMMONING verse).
	// C'est ici qu'entrent les voies d'exception que le texte ne dit pas.
	std::vector<Requirement> lift_mats;
	auto watch = [&](const Message& m) {
		if(!recipes)
			return;
		if(m.type == MSG_MOVE && m.size >= 28) {
			uint32_t code = 0, reason = 0;
			std::memcpy(&code, m.data, 4);
			std::memcpy(&reason, m.data + 24, 4);
			if(reason & REASON_MATERIAL)
				lift_mats.push_back(
					Requirement{ duel.Db().Canonical(code),
								 NormalizeZone(m.data[5]) });
		} else if((m.type == MSG_SUMMONING || m.type == MSG_SPSUMMONING) &&
				  m.size >= 4) {
			uint32_t code = 0;
			std::memcpy(&code, m.data, 4);
			if(code)
				recipes->Observe(duel.Db().Canonical(code), lift_mats);
			lift_mats.clear();
		}
	};

	// Meme appariement que LiftPlan : on identifie la reponse enregistree par
	// l'ETAT qu'elle atteint, jamais octet par octet (l'encodage d'EDOPro et
	// celui de l'enumerateur different). 0 = rejet par le core.
	auto advance = [&](const std::vector<uint8_t>& resp) -> uint64_t {
		duel.SetResponse(resp);
		uint8_t nt = 0;
		std::vector<uint8_t> np;
		for(;;) {
			int status = duel.Process();
			for(const Message& m : duel.Messages()) {
				if(m.type == MSG_RETRY)
					return 0;
				watch(m);
				if(IsPrompt(m.type)) {
					nt = m.type;
					np.assign(m.data, m.data + m.size);
					// Frontiere de pas : des materiaux restes sans invocation
					// ne doivent pas se coller a la suivante (meme semantique
					// que StepToPrompt, qui repart a vide a chaque pas).
					lift_mats.clear();
				}
			}
			if(status != OCG_DUEL_STATUS_CONTINUE)
				break;
		}
		return StateDigest(duel, nt, np);
	};

	for(;;) {
		int status = duel.Process();
		for(const Message& m : duel.Messages()) {
			watch(m);
			if(IsPrompt(m.type)) {
				ptype = m.type;
				payload.assign(m.data, m.data + m.size);
				player = m.size ? m.data[0] : -1;
				lift_mats.clear();
			}
		}
		if(status == OCG_DUEL_STATUS_END)
			break;
		if(status != OCG_DUEL_STATUS_AWAITING)
			continue;
		if(ri >= yrp.responses.size() || ri >= stop_after)
			break;

		const std::vector<uint8_t>& recorded = yrp.responses[ri];
		// LANDMARKS (chantier 18) : le multiensemble des faits AVANT de jouer
		// cette decision. On enregistre, pour chaque fait, l'indice ou son
		// k-ieme exemplaire est apparu POUR LA PREMIERE FOIS — c'est de la que
		// sortent a la fois le compte (la boucle de repetition du papier) et
		// l'ordre (les aretes de progression).
		//
		// Le releve porte sur TOUTES les decisions du joueur cible, y compris
		// les prompts a choix unique : un fait devient vrai pendant une
		// resolution, pas seulement a un carrefour. Le filtrer sur les
		// multi-choix aurait rate exactement les faits qui arrivent « tout
		// seuls » — ceux qu'une macro absorbe.
		if(landmarks && player == target_player) {
			static thread_local std::unordered_map<uint64_t, uint32_t> facts;
			CollectStateFacts(duel, static_cast<uint8_t>(target_player), facts);
			if(landmarks->decisions == 0)
				landmarks->initial = facts;
			for(const auto& [k, n] : facts) {
				std::vector<float>& v = landmarks->first[k];
				while(v.size() < n)
					v.push_back(static_cast<float>(landmarks->decisions));
			}
			++landmarks->decisions;
		}
		if(player == target_player) {
			auto choices = Enumerate(ptype, payload.data(),
									 static_cast<uint32_t>(payload.size()), eo);
			// Une decision a choix unique n'apprend rien : le gradient y vaut
			// alpha - alpha*1 = 0. C'est aussi la convention de PolicyRollout,
			// qui n'enregistre un PolicyStep qu'a partir de deux choix.
			if(choices.size() > 1) {
				arena.Push();
				uint64_t want = advance(recorded);
				arena.Restore();
				size_t pick = choices.size();
				for(size_t i = 0; i < choices.size(); ++i) {
					uint64_t got = advance(choices[i].response);
					arena.Restore();
					if(want && got == want) {
						pick = i;
						break;
					}
				}
				arena.Pop();
				if(pick < choices.size()) {
					PolicyStep step;
					// Le CONTEXTE, calcule comme au tirage : cartes du board
					// cible deja posees a cet instant de la ligne.
					ComputeBoardKeyInto(duel,
										static_cast<uint8_t>(target_player),
										here);
					step.ctx = ContextKey(
						CommonCodes(here.codes, target.codes),
						duel.Count(static_cast<uint8_t>(target_player),
								   LOCATION_HAND));
					step.keys.reserve(choices.size());
					step.known.reserve(choices.size());
					for(const Choice& c : choices) {
						step.keys.push_back(c.plan_key);
						// Le biais du repertoire est reproduit ici parce que
						// Adapt() doit recalculer les MEMES probabilites que
						// l'echantillonnage (cf. PolicyRollout : un changement
						// de phase est au repertoire sans y meriter de biais).
						step.known.push_back(
							(repertoire.count(c.plan_key) != 0 && !c.phase) ? 1
																			: 0);
					}
					step.chosen = pick;
					step.cctx = mcps_depth ? path_ctx
										   : static_cast<uint64_t>(step.ctx);
					if(mcps_depth && nsteps < mcps_depth)
						path_ctx += MixMove(choices[pick].plan_key);
					++nsteps;
					out.steps.push_back(std::move(step));
				} else {
					++unknown;
					// Une etape non identifiee ROMPT le chemin : on ne sait pas
					// quel coup la ligne a joue, donc on ne peut pas l'ajouter.
					// Le contexte des etapes suivantes ne correspondra plus a
					// celui d'un tirage qui aurait joue la meme ligne — c'est
					// une sous-estimation assumee, comme le saut lui-meme.
					++nsteps;
				}
			}
		}
		duel.SetResponse(recorded);
		++ri;
	}
	return unknown;
}

double CorpusAgreement(const NrpaPolicy& pol, const NrpaResidual* res,
					   const std::vector<NrpaRun>& runs, float bias_known,
					   float shrink, double* argmax_frac) {
	double total = 0;
	size_t n = 0, top = 0;
	std::vector<double> p;
	for(const NrpaRun& r : runs) {
		for(const PolicyStep& s : r.steps) {
			p.resize(s.keys.size());
			double mx = -1e300;
			for(size_t i = 0; i < s.keys.size(); ++i) {
				double w = EffectiveWeight(pol, res, s.keys[i], s.cctx, shrink);
				p[i] = w + (s.known[i] ? bias_known : 0.0f);
				mx = (std::max)(mx, p[i]);
			}
			double sum = 0;
			for(double& x : p) { x = std::exp(x - mx); sum += x; }
			total += std::log((std::max)(p[s.chosen] / sum, 1e-30));
			bool first = true;
			for(size_t i = 0; i < p.size(); ++i)
				if(i != s.chosen && p[i] > p[s.chosen]) { first = false; break; }
			top += first ? 1 : 0;
			++n;
		}
	}
	if(argmax_frac)
		*argmax_frac = n ? static_cast<double>(top) / static_cast<double>(n) : 0.0;
	return n ? total / static_cast<double>(n) : 0.0;
}

CostForecast ForecastSearchCost(const NrpaPolicy& pol, const NrpaResidual* res,
								const std::vector<NrpaRun>& runs,
								float bias_known, float shrink) {
	CostForecast out;
	std::vector<double> p;
	double sum_mono = 0, sum_decomp = 0, sum_q = 0, sum_worst = 0;
	for(const NrpaRun& r : runs) {
		if(r.steps.empty())
			continue;
		// Segment courant : profondeur et log-probabilite cumulees depuis le
		// dernier indice. Un indice = la composante « cartes posees » du
		// contexte augmente (une sous-tache du but conjonctif vient de tomber).
		double seg_logpi = 0, total_logpi = 0;
		uint32_t seg_depth = 0, total_depth = 0;
		uint16_t last_placed = static_cast<uint16_t>(r.steps.front().ctx / 16);
		// Bornes accumulees : somme des d_i/pi_i, en log10 par segment puis
		// somme en lineaire via le max (les termes couvrent des ordres de
		// grandeur enormes, sommer naivement perdrait tout).
		double best_log10 = -1e300, worst_log10 = -1e300;
		std::vector<double> seg_log10;
		auto close_segment = [&] {
			if(!seg_depth)
				return;
			const double l = std::log10(static_cast<double>(seg_depth)) -
							 seg_logpi / std::log(10.0);
			seg_log10.push_back(l);
			worst_log10 = (std::max)(worst_log10, l);
			best_log10 = (std::max)(best_log10, l);
			seg_logpi = 0;
			seg_depth = 0;
		};
		for(const PolicyStep& s : r.steps) {
			const uint16_t placed = static_cast<uint16_t>(s.ctx / 16);
			if(placed != last_placed) {
				close_segment();
				last_placed = placed;
			}
			p.resize(s.keys.size());
			double mx = -1e300;
			for(size_t i = 0; i < s.keys.size(); ++i) {
				double w = EffectiveWeight(pol, res, s.keys[i], s.cctx, shrink);
				p[i] = w + (s.known[i] ? bias_known : 0.0f);
				mx = (std::max)(mx, p[i]);
			}
			double sm = 0;
			for(double& x : p) { x = std::exp(x - mx); sm += x; }
			const double lp = std::log((std::max)(p[s.chosen] / sm, 1e-300));
			seg_logpi += lp;
			total_logpi += lp;
			++seg_depth;
			++total_depth;
		}
		close_segment();
		if(seg_log10.empty())
			continue;
		// Somme des bornes par segment, en log10 stable (log-sum-exp base 10).
		double acc = 0;
		for(double l : seg_log10)
			acc += std::pow(10.0, l - worst_log10);
		const double decomp = worst_log10 + std::log10(acc);
		const double mono = std::log10(static_cast<double>(total_depth)) -
							total_logpi / std::log(10.0);
		sum_mono += mono;
		sum_decomp += decomp;
		sum_q += static_cast<double>(seg_log10.size());
		sum_worst += worst_log10;
		++out.lines;
	}
	if(out.lines) {
		const double n = static_cast<double>(out.lines);
		out.log10_mono = sum_mono / n;
		out.log10_decomp = sum_decomp / n;
		out.segments = sum_q / n;
		out.worst_seg_log10 = sum_worst / n;
	}
	return out;
}

OptionForecast ForecastOptionGain(const std::vector<NrpaRun>& runs,
								  size_t max_options, uint32_t min_support,
								  size_t max_len) {
	OptionForecast out;
	// Les lignes du corpus, reduites a la SEQUENCE DES COUPS JOUES. C'est sur
	// elle que se minent les macros, et sur elle que se mesure la substitution.
	std::vector<std::vector<uint64_t>> played;
	std::vector<std::vector<uint32_t>> legal;
	for(const NrpaRun& r : runs) {
		if(r.steps.empty())
			continue;
		played.emplace_back();
		legal.emplace_back();
		for(const PolicyStep& s : r.steps) {
			played.back().push_back(s.keys[s.chosen]);
			legal.back().push_back(
				static_cast<uint32_t>((std::max)(s.keys.size(), size_t{ 1 })));
		}
	}
	if(played.empty())
		return out;

	// MINAGE. Support d'une sous-sequence = nombre d'occurrences dans tout le
	// corpus. Une macro ne vaut d'etre retenue que si elle revient : une
	// sous-sequence vue une fois est une ligne, pas une option.
	std::map<std::vector<uint64_t>, uint32_t> support;
	for(const std::vector<uint64_t>& seq : played)
		for(size_t i = 0; i < seq.size(); ++i)
			for(size_t len = 2; len <= max_len && i + len <= seq.size(); ++len)
				++support[std::vector<uint64_t>(seq.begin() + i,
												seq.begin() + i + len)];

	// SELECTION. Gain brut d'une macro = (len - 1) decisions economisees par
	// occurrence. On trie la-dessus et on prend les `max_options` premieres :
	// une selection gloutonne exacte (re-evaluer la perte apres chaque ajout)
	// couterait un ordre de grandeur de plus pour un instrument dont le role
	// est de dire OUI ou NON, pas de livrer le catalogue definitif.
	std::vector<std::pair<double, std::vector<uint64_t>>> ranked;
	for(const auto& [seq, n] : support) {
		if(n < min_support)
			continue;
		ranked.emplace_back(static_cast<double>(n) *
								static_cast<double>(seq.size() - 1),
							seq);
	}
	std::sort(ranked.begin(), ranked.end(),
			  [](const auto& a, const auto& b) {
				  if(a.first != b.first) return a.first > b.first;
				  return a.second.size() > b.second.size();
			  });
	std::vector<std::vector<uint64_t>> catalog;
	for(const auto& [gain, seq] : ranked) {
		if(catalog.size() >= max_options)
			break;
		catalog.push_back(seq);
		out.max_len = (std::max)(out.max_len, seq.size());
	}
	out.options = catalog.size();
	if(catalog.empty())
		return out;

	// SUBSTITUTION ET COUT. La correspondance est GLOUTONNE, la plus longue
	// d'abord : c'est ce que ferait un enumerateur qui propose ses macros.
	const double m = static_cast<double>(catalog.size());
	double sum_flat = 0, sum_opt = 0, sum_dflat = 0, sum_dopt = 0;
	size_t absorbed = 0, total_steps = 0;
	for(size_t li = 0; li < played.size(); ++li) {
		const std::vector<uint64_t>& seq = played[li];
		double logpi_flat = 0, logpi_opt = 0;
		size_t d_opt = 0;
		for(size_t i = 0; i < seq.size();) {
			// Cout sans options : une decision, parmi ses legaux.
			logpi_flat += std::log10(static_cast<double>(legal[li][i]));
			size_t best = 0;
			for(const std::vector<uint64_t>& opt : catalog) {
				if(opt.size() <= best || i + opt.size() > seq.size())
					continue;
				if(std::equal(opt.begin(), opt.end(), seq.begin() + i))
					best = opt.size();
			}
			// Le denominateur porte le catalogue ENTIER : on suppose toutes les
			// options proposees partout, ce qui MINORE le gain.
			logpi_opt += std::log10(static_cast<double>(legal[li][i]) + m);
			++d_opt;
			if(best) {
				absorbed += best;
				// Les decisions absorbees gardent leur cout DANS le bras plat,
				// qui doit rester la ligne complete.
				for(size_t k = 1; k < best; ++k)
					logpi_flat +=
						std::log10(static_cast<double>(legal[li][i + k]));
				i += best;
			} else {
				++i;
			}
		}
		total_steps += seq.size();
		sum_flat += std::log10(static_cast<double>(seq.size())) + logpi_flat;
		sum_opt += std::log10(static_cast<double>(d_opt)) + logpi_opt;
		sum_dflat += static_cast<double>(seq.size());
		sum_dopt += static_cast<double>(d_opt);
		++out.lines;
	}
	if(out.lines) {
		const double n = static_cast<double>(out.lines);
		out.log10_flat = sum_flat / n;
		out.log10_opt = sum_opt / n;
		out.depth_flat = sum_dflat / n;
		out.depth_opt = sum_dopt / n;
		out.covered = total_steps ? static_cast<double>(absorbed) /
										static_cast<double>(total_steps)
								  : 0.0;
	}
	return out;
}

// Catalogue EXECUTABLE d'options (chantier 17, v3 apres l'audit) : minage des
// sous-sequences du corpus, puis selection GLOUTONNE PAR PERTE DE LEVIN au
// lieu du gain brut — le critere du papier (2410.11262). Le modele evalue,
// sous politique uniforme, Sigma log10(b_i + proposables_i) le long de chaque
// ligne avec la MEILLEURE segmentation (programmation dynamique) : une macro
// paie sa presence au denominateur PARTOUT ou elle est proposable, et ne
// crédite que la ou la ligne l'a jouee. La selection s'arrete quand plus
// aucun candidat n'ameliore la perte : la taille du catalogue est un RESULTAT,
// pas un parametre — max_options n'est qu'un plafond.
OptionCatalog MineOptionCatalog(const std::vector<NrpaRun>& runs,
								size_t max_options, uint32_t min_support,
								size_t max_len, uint32_t window, int ctx_tol) {
	OptionCatalog cat;
	cat.window = window;
	cat.ctx_tol = ctx_tol;
	// 1. Les lignes : coup joue, largeur legale, cles legales et contexte
	// semantique par decision.
	struct Line {
		std::vector<uint64_t> played;
		std::vector<uint32_t> legal;
		std::vector<const std::vector<uint64_t>*> keys;
		std::vector<uint16_t> ctx;
	};
	std::vector<Line> lines;
	for(const NrpaRun& r : runs) {
		if(r.steps.empty())
			continue;
		lines.emplace_back();
		for(const PolicyStep& s : r.steps) {
			lines.back().played.push_back(s.keys[s.chosen]);
			lines.back().legal.push_back(static_cast<uint32_t>(
				(std::max)(s.keys.size(), size_t{ 1 })));
			lines.back().keys.push_back(&s.keys);
			lines.back().ctx.push_back(s.ctx);
		}
	}
	if(lines.empty())
		return cat;

	// 2. Le vivier : sous-sequences a support >= min_support, position
	// moyenne d'occurrence, contextes de depart d'occurrence, plafonne aux
	// 1024 meilleures au gain brut (le glouton exact sur tout le treillis
	// couterait un ordre de plus).
	struct Cand {
		std::vector<uint64_t> seq;
		uint32_t n = 0;
		uint32_t pos = 0;
		double brut = 0;
		std::vector<uint16_t> ctxs;
	};
	struct SupEntry {
		uint32_t n = 0;
		uint64_t pos = 0;
		std::vector<uint16_t> ctxs;
	};
	std::map<std::vector<uint64_t>, SupEntry> sup;
	for(const Line& L : lines)
		for(size_t i = 0; i < L.played.size(); ++i)
			for(size_t len = 2; len <= max_len && i + len <= L.played.size();
				++len) {
				auto& e = sup[std::vector<uint64_t>(
					L.played.begin() + i, L.played.begin() + i + len)];
				++e.n;
				e.pos += i;
				e.ctxs.push_back(L.ctx[i]);
			}
	std::vector<Cand> pool;
	for(auto& [seq, e] : sup) {
		if(e.n < min_support)
			continue;
		std::sort(e.ctxs.begin(), e.ctxs.end());
		e.ctxs.erase(std::unique(e.ctxs.begin(), e.ctxs.end()), e.ctxs.end());
		pool.push_back({ seq, e.n, static_cast<uint32_t>(e.pos / e.n),
						 static_cast<double>(e.n) *
							 static_cast<double>(seq.size() - 1),
						 std::move(e.ctxs) });
	}
	std::sort(pool.begin(), pool.end(), [](const Cand& a, const Cand& b) {
		if(a.brut != b.brut) return a.brut > b.brut;
		return a.seq.size() > b.seq.size();
	});
	constexpr size_t kPool = 1024;
	if(pool.size() > kPool)
		pool.resize(kPool);
	if(pool.empty())
		return cat;

	// 3. Pre-calculs par position globale : ou chaque candidat est PROPOSABLE
	// (premiere cle legale + fenetre — le meme test que le rollout) et ou il
	// est JOUE (la ligne a joue exactement sa sequence ici).
	std::vector<size_t> line_base(lines.size());
	size_t total = 0;
	for(size_t li = 0; li < lines.size(); ++li) {
		line_base[li] = total;
		total += lines[li].played.size();
	}
	std::unordered_map<uint64_t, std::vector<uint32_t>> cand_by_first;
	for(size_t ci = 0; ci < pool.size(); ++ci)
		cand_by_first[pool[ci].seq.front()].push_back(
			static_cast<uint32_t>(ci));
	auto in_window = [&](const Cand& c, size_t i) {
		if(!window)
			return true;
		const uint32_t p = c.pos;
		return i + window >= p && i <= size_t{ p } + window;
	};
	// La garde semantique du modele de selection : le MEME test que le
	// rollout (OptionCatalog::CtxOk), sans quoi la perte modele serait
	// calculee sur un denominateur que le run ne paie pas.
	auto ctx_ok = [&](const Cand& c, uint16_t ctx) {
		if(ctx_tol < 0)
			return true;
		for(uint16_t w : c.ctxs)
			if(OptionCtxCompatible(ctx, w, static_cast<uint32_t>(ctx_tol)))
				return true;
		return false;
	};
	std::vector<std::vector<uint32_t>> proposable(total), playable(total);
	for(size_t li = 0; li < lines.size(); ++li) {
		const Line& L = lines[li];
		for(size_t i = 0; i < L.played.size(); ++i) {
			const size_t g = line_base[li] + i;
			for(uint64_t k : *L.keys[i]) {
				auto it = cand_by_first.find(k);
				if(it == cand_by_first.end())
					continue;
				for(uint32_t ci : it->second)
					if(in_window(pool[ci], i) && ctx_ok(pool[ci], L.ctx[i]) &&
					   (proposable[g].empty() || proposable[g].back() != ci))
						proposable[g].push_back(ci);
			}
			auto it = cand_by_first.find(L.played[i]);
			if(it == cand_by_first.end())
				continue;
			for(uint32_t ci : it->second) {
				const auto& s = pool[ci].seq;
				if(in_window(pool[ci], i) && ctx_ok(pool[ci], L.ctx[i]) &&
				   i + s.size() <= L.played.size() &&
				   std::equal(s.begin(), s.end(), L.played.begin() + i))
					playable[g].push_back(ci);
			}
		}
	}

	// 4. Selection gloutonne. prop_count[g] = proposables du catalogue
	// courant a la position g ; l'evaluation d'un candidat ajoute son propre
	// bit a la volee. Faisceau : les 64 meilleurs candidats bruts restants.
	std::vector<uint32_t> prop_count(total, 0);
	std::vector<char> chosen_mask(pool.size(), 0);
	std::vector<double> cost;
	auto loss = [&](int extra) -> double {
		double sum = 0;
		for(size_t li = 0; li < lines.size(); ++li) {
			const Line& L = lines[li];
			const size_t n = L.played.size();
			cost.assign(n + 1, 0.0);
			for(size_t i = n; i-- > 0;) {
				const size_t g = line_base[li] + i;
				uint32_t extra_here = 0;
				if(extra >= 0)
					for(uint32_t ci : proposable[g])
						if(ci == static_cast<uint32_t>(extra)) {
							extra_here = 1;
							break;
						}
				const double denom = std::log10(static_cast<double>(
					L.legal[i] + prop_count[g] + extra_here));
				double best = denom + cost[i + 1];
				for(uint32_t ci : playable[g])
					if(chosen_mask[ci] ||
					   (extra >= 0 && ci == static_cast<uint32_t>(extra))) {
						const double c =
							denom + cost[i + pool[ci].seq.size()];
						best = (std::min)(best, c);
					}
				cost[i] = best;
			}
			sum += cost[0];
		}
		return sum / static_cast<double>(lines.size());
	};
	cat.model_flat = loss(-1);
	double current = cat.model_flat;
	std::vector<uint32_t> chosen;
	constexpr size_t kBeam = 64;
	while(chosen.size() < max_options) {
		double best_loss = current;
		int best_ci = -1;
		size_t seen = 0;
		for(size_t ci = 0; ci < pool.size() && seen < kBeam; ++ci) {
			if(chosen_mask[ci])
				continue;
			++seen;
			const double l = loss(static_cast<int>(ci));
			if(l < best_loss) {
				best_loss = l;
				best_ci = static_cast<int>(ci);
			}
		}
		// Plus aucun candidat du faisceau n'ameliore la perte : le catalogue
		// a atteint sa taille NATURELLE.
		if(best_ci < 0)
			break;
		chosen_mask[best_ci] = 1;
		chosen.push_back(static_cast<uint32_t>(best_ci));
		current = best_loss;
		for(size_t g = 0; g < total; ++g)
			for(uint32_t ci : proposable[g])
				if(ci == static_cast<uint32_t>(best_ci)) {
					++prop_count[g];
					break;
				}
	}
	cat.model_opt = current;

	// 5. Le catalogue, dans l'ordre de selection (front() = la meilleure).
	for(uint32_t ci : chosen) {
		cat.seqs.push_back(pool[ci].seq);
		cat.pos.push_back(pool[ci].pos);
		cat.ctxs.push_back(pool[ci].ctxs);
	}
	for(size_t m = 0; m < cat.seqs.size(); ++m) {
		// Identite de la macro dans l'espace des poids. Collision id/plan_key
		// du meme ordre qu'entre deux plan_key : negligee des deux cotes.
		uint64_t id = 0x9E3779B97F4A7C15ull;
		for(uint64_t k : cat.seqs[m])
			id = (id ^ k) * 0x100000001B3ull;
		cat.ids.push_back(id);
		cat.by_first[cat.seqs[m].front()].push_back(static_cast<uint32_t>(m));
	}
	return cat;
}

double CorpusCoherence(const std::vector<NrpaRun>& runs, bool use_ctx,
					   size_t* groups) {
	// signature du point de decision -> (coup joue -> nombre de fois)
	std::unordered_map<uint64_t, std::unordered_map<uint64_t, uint32_t>> seen;
	std::vector<uint64_t> sorted;
	size_t total = 0;
	for(const NrpaRun& r : runs) {
		for(const PolicyStep& s : r.steps) {
			sorted = s.keys;
			std::sort(sorted.begin(), sorted.end());
			uint64_t sig = use_ctx ? (s.cctx + 1) * 0x9e3779b97f4a7c15ull : 0;
			for(uint64_t k : sorted)
				sig = (sig ^ k) * 0x100000001b3ull;
			++seen[sig][s.keys[s.chosen]];
			++total;
		}
	}
	if(groups)
		*groups = seen.size();
	if(!total)
		return 0.0;
	size_t majority = 0;
	for(const auto& [sig, choices] : seen) {
		uint32_t best = 0;
		for(const auto& [k, n] : choices)
			best = (std::max)(best, n);
		majority += best;
	}
	return static_cast<double>(majority) / static_cast<double>(total);
}

void LiftRefLine(Duel& duel, Arena& arena, const Replay& yrp, int target_player,
				 size_t stop_after, const EnumOptions& eo,
				 std::unordered_map<uint64_t, size_t>& digests,
				 std::vector<uint64_t>& keys) {
	uint8_t ptype = 0;
	std::vector<uint8_t> payload;
	int player = -1;
	size_t ri = 0;
	keys.assign((std::min)(yrp.responses.size(), stop_after), 0);

	// Meme appariement que LiftPlan : applique une reponse, rend l'empreinte de
	// l'etat atteint (0 = rejet, aucune correspondance possible).
	auto advance = [&](const std::vector<uint8_t>& resp) -> uint64_t {
		duel.SetResponse(resp);
		uint8_t nt = 0;
		std::vector<uint8_t> np;
		for(;;) {
			int status = duel.Process();
			for(const Message& m : duel.Messages()) {
				if(m.type == MSG_RETRY)
					return 0;
				if(IsPrompt(m.type)) {
					nt = m.type;
					np.assign(m.data, m.data + m.size);
				}
			}
			if(status != OCG_DUEL_STATUS_CONTINUE)
				break;
		}
		return StateDigest(duel, nt, np);
	};

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
			continue;
		if(ri >= yrp.responses.size() || ri >= stop_after)
			break;
		// Premiere occurrence conservee — sans objet en pratique : les etats de
		// la reference sont deux a deux distincts (le rapport verifie 0 fusion).
		digests.emplace(StateDigest(duel, ptype, payload), ri);

		const std::vector<uint8_t>& recorded = yrp.responses[ri];
		if(player == target_player) {
			auto choices = Enumerate(ptype, payload.data(),
									 static_cast<uint32_t>(payload.size()), eo);
			arena.Push();
			uint64_t want = advance(recorded);
			arena.Restore();
			for(const Choice& c : choices) {
				uint64_t got = advance(c.response);
				arena.Restore();
				if(want && got == want) {
					keys[ri] = c.plan_key;
					break;
				}
			}
			arena.Pop();
		}
		duel.SetResponse(recorded);
		++ri;
	}
}

void Search::RunRepair(const BoardKey& t, uint32_t discrepancies) {
	prof::Scope ps(prof::kSearch);
	target = t;
	start = std::chrono::steady_clock::now();
	solutions.clear();
	path.clear();
	tt.clear();
	novelty.Clear();
	cfg_discrepancies = discrepancies;
	DescendRepair(0, 0, 0, discrepancies, 0, 0, 0, 0);
	stats.ms = std::chrono::duration<double, std::milli>(
				   std::chrono::steady_clock::now() - start).count();
	stats.hit_node_limit = stats.nodes >= cfg.max_nodes;
	stats.exhausted = !stats.hit_time_limit && !stats.hit_node_limit;
	stats.novelty_atoms = novelty.Size();
}

void Search::RunGuided(const BoardKey& t) {
	prof::Scope ps(prof::kSearch);
	target = t;
	start = std::chrono::steady_clock::now();
	solutions.clear();
	path.clear();
	tt.clear();
	DescendGuided(0, 0, cfg.initial_turns, cfg.initial_summons,
				  cfg.initial_resolved);
	stats.ms = std::chrono::duration<double, std::milli>(
				   std::chrono::steady_clock::now() - start).count();
	stats.hit_node_limit = stats.nodes >= cfg.max_nodes;
	stats.exhausted = !stats.hit_time_limit && !stats.hit_node_limit;
}

void Search::Run(const BoardKey& t) {
	prof::Scope ps(prof::kSearch);
	target = t;
	start = std::chrono::steady_clock::now();
	solutions.clear();
	path.clear();
	tt.clear();
	Descend(0, 0);
	stats.ms = std::chrono::duration<double, std::milli>(
				   std::chrono::steady_clock::now() - start).count();
	stats.exhausted = !stats.hit_time_limit && stats.nodes < cfg.max_nodes;
	stats.hit_node_limit = stats.nodes >= cfg.max_nodes;
}

} // namespace solver
