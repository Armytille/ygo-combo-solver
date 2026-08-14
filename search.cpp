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
	key.codes.clear();
	key.mzone_count = 0;
	for(uint32_t loc : { LOCATION_MZONE, LOCATION_SZONE }) {
		duel.Query(con, loc, kBoardFlags, cards);
		for(const auto& c : cards) {
			if(!c.present)
				continue;
			key.entries.push_back(EntryOf(loc, c, duel.Db(), /*goal_view=*/true));
			key.codes.push_back(c.Code());
			if(loc == LOCATION_MZONE)
				++key.mzone_count;
		}
	}
	// Tri : deux boards identiques a permutation de colonnes pres doivent
	// donner la meme cle (criterium d'equivalence retenu, cf. section 1).
	std::sort(key.entries.begin(), key.entries.end());
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
		key.codes.push_back(c.Code());
		++key.mzone_count;
	}
	for(const auto& c : szone) {
		if(!c.present)
			continue;
		key.entries.push_back(EntryOf(LOCATION_SZONE, c, db, /*goal_view=*/true));
		key.codes.push_back(c.Code());
	}
	std::sort(key.entries.begin(), key.entries.end());
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
	return StateDigest(const_cast<Duel&>(duel), prompt_type, prompt_payload);
}

bool Search::FillChoices(ChoiceList& out) {
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

uint64_t StateDigest(Duel& d, uint8_t prompt_type,
					 const std::vector<uint8_t>& prompt_payload) {
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

void Search::Descend(uint32_t depth, uint32_t actions) {
	if(BudgetExhausted()) {
		stats.hit_time_limit = true;
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
	// borne. Comptees, sans quoi "EPUISE" mentirait (C2).
	if(depth >= cfg.max_decisions) {
		++stats.edges_skipped;
		return;
	}
	if(cfg.max_actions && total_actions >= cfg.max_actions) {
		++stats.edges_skipped;
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
	if(fresh && depth < stats.distinct_by_depth.size())
		++stats.distinct_by_depth[depth];

	ChoiceList& choices = ChoicesAt(depth);
	if(!FillChoices(choices)) {
		++stats.dead_ends;
		return;
	}

	arena.Push();
	for(const Choice& c : choices) {
		duel.SetResponse(c.response);
		path.push_back(c.response);
		Descend(depth + 1, total_actions);
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
float Search::RecipeDistance(const BoardKey& here, uint64_t resolved) {
	if(!cfg.recipes)
		return 0.0f;
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
	// Combien d'entites presentes satisfont une exigence. Appele une fois par
	// materiau de chaque recette exploree : recherche binaire sur la table triee
	// pour une carte nommee, parcours pour une exigence cardinale (la table fait
	// ~60-80 entrees et n'est triee ni sur l'archetype ni sur le niveau).
	auto avail = [this](const Requirement& r) -> uint32_t {
		// EXIGENCE CARDINALE : un COMPTE, pas un test de presence. La table
		// n'est pas triee sur l'archetype ni sur le niveau — on la parcourt, et
		// elle est courte. Le DECK et l'EXTRA sont exclus par ZoneIsPlayable :
		// trois Lunalight qui dorment dans le deck ne sont pas trois materiaux.
		if(r.kind != kReqCard) {
			uint32_t n = 0;
			for(const PresentInfo& pi : recipe_present_info) {
				if(!pi.row || !(pi.row->type & kRecipeTypeMonster))
					continue;
				if(r.zone == kZoneAny ? !ZoneIsPlayable(pi.zone)
									  : pi.zone != r.zone)
					continue;
				if(r.kind == kReqLevel) {
					n += (pi.row->level & 0xff) == r.code ? 1u : 0u;
				} else {
					for(uint16_t sc : pi.row->setcodes)
						if(sc && SetcodeMatches(sc,
												static_cast<uint16_t>(r.code))) {
							++n;
							break;
						}
				}
			}
			return n;
		}
		auto by_code = [](const Requirement& a, const Requirement& b) {
			if(a.code != b.code) return a.code < b.code;
			return a.zone < b.zone;
		};
		auto it = std::lower_bound(recipe_present.begin(), recipe_present.end(),
								   Requirement{ r.code, 0 }, by_code);
		if(it == recipe_present.end() || it->code != r.code)
			return 0u;
		// Zone JOKER (exigence amorcee par le texte) : n'importe quelle zone ou
		// un materiau se PREND. Le deck et l'extra n'en sont pas : une carte qui
		// y dort doit encore etre invoquee, et c'est l'etape que l'on compte.
		for(; it != recipe_present.end() && it->code == r.code; ++it) {
			if(r.zone == kZoneAny ? ZoneIsPlayable(it->zone)
								  : it->zone == r.zone)
				return 1u;
		}
		return 0u;
	};

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
	// Les resolutions exigees restent comptees comme avant : le graphe ne
	// modelise que les INVOCATIONS.
	(void)resolved;
	return static_cast<float>(total);
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
	bool alt_hit = false;
	if(!(here == target)) {
		if(!cfg.target_alts)
			return false;
		for(const BoardKey& a : *cfg.target_alts)
			if(here == a) {
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
	// Cas courant gratuit : le score OPTIMISTE (0 brulees) sous le plancher
	// evite les deux requetes de zone du compte reel.
	uint64_t score = pack(0);
	if(archive.size() >= cfg.archive_k && score <= archive_min_score)
		return;
	if(cfg.anytime) {
		burned = CurrentBurned();
		score = pack(burned);
		if(archive.size() >= cfg.archive_k && score <= archive_min_score)
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
		for(size_t i = 1; i < archive.size(); ++i)
			if(archive[i].score < archive[worst].score)
				worst = i;
		if(score <= archive[worst].score)
			return;
		archive_cells.erase(archive[worst].cell);
		archive_cells.emplace(here.hash, worst);
		archive[worst] = { here.hash, score, overlap, rp, depth, burned, path };
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
	// Compteurs initiaux : le finisseur echantillonne depuis un etat de recul
	// deja profond — contraintes d'invocation, coupure de tour et gradient de
	// resolutions doivent compter depuis le prefixe, pas depuis zero.
	uint32_t actions = 0, turns = cfg.initial_turns,
			 summons = cfg.initial_summons;
	uint64_t resolved = cfg.initial_resolved;
	uint32_t rp_prev = ResolveProgress(resolved);
	double novel_states = 0;
	std::vector<double> logit;

	for(uint32_t depth = 0; depth < cfg.max_decisions; ++depth) {
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
		summons += static_cast<uint32_t>(summons_this_step.size());
		resolved += resolved_this_step;
		++stats.nodes;
		prof::Count(prof::kDecisions);
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
				CollectAtoms(duel, static_cast<uint8_t>(cfg.target_player), here,
							 partition, atoms_scratch);
				if(novelty.Observe(atoms_scratch, depth)) {
					++stats.novelty_novel;
					novel_states += 1;
				} else {
					++stats.novelty_stale;
				}
			}
			// Les resolutions exigees (--resolve) pesent PLUS que des cartes
			// cibles (cfg.resolve_weight) : a poids egal, les lignes 8/8 sans
			// rip gagnaient la course d'adaptation contre les rip-partielles.
			double material = static_cast<double>(Heuristic(here)) +
							  static_cast<double>(cfg.resolve_weight) *
								  ResolveProgress(resolved);
			double sc = material * 1000.0 + novel_states;
			if(sc > run.score)
				run.score = sc;
		}

		ChoiceList& choices = ro_choices;
		if(!FillChoices(choices)) {
			++stats.dead_ends;
			return;
		}

		size_t pick = 0;
		if(choices.size() > 1) {
			// Echantillonnage softmax sous la politique — AUCUNE evaluation des
			// fils : c'est ce qui rend un tirage NRPA plusieurs fois moins cher
			// qu'un tirage glouton (qui avance/mesure/restaure chaque fils).
			PolicyStep step;
			// CONTEXTE de la decision (chantier 5ter) : cartes du board cible
			// deja posees. Le meme descripteur que celui releve sur les lignes
			// de corpus — il est SEMANTIQUE, donc comparable d'une ligne a
			// l'autre, contrairement a la profondeur.
			// Le descripteur coute une requete de zone par decision : il ne se
			// paie que si le niveau contextuel est allume.
			if(cfg.ctx_shrink >= 0.0f)
				step.ctx = ContextKey(
					CommonCodes(here.codes, target.codes),
					duel.Count(static_cast<uint8_t>(cfg.target_player),
							   LOCATION_HAND));
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
				double w = EffectiveWeight(pol, &ctx_weights, key, step.ctx,
										   cfg.ctx_shrink);
				logit[i] = (w + (known ? cfg.nrpa_bias_known : 0.0f) +
							(hinted ? cfg.hint_bias : 0.0f)) /
						   (cfg.nrpa_temp > 1e-3f ? cfg.nrpa_temp : 1e-3f);
				mx = (std::max)(mx, logit[i]);
				step.keys.push_back(key);
				step.known.push_back(known ? 1 : 0);
				step.hinted.push_back(hinted ? 1 : 0);
			}
			double sum = 0;
			for(double& x : logit) { x = std::exp(x - mx); sum += x; }
			double u = double(next() >> 11) * (1.0 / 9007199254740992.0) * sum;
			double acc = 0;
			for(size_t i = 0; i < logit.size(); ++i) {
				acc += logit[i];
				if(u < acc || i + 1 == logit.size()) { pick = i; break; }
			}
			// Visibilite des indices : un coup indice etait-il seulement LEGAL
			// ici ? C'est la mesure qui separe "mal echantillonne" de "jamais
			// propose par le core".
			if(!cfg.hint_cards.empty()) {
				bool any = false;
				for(uint8_t h : step.hinted)
					any |= h != 0;
				if(any) {
					++stats.hint_seen;
					if(step.hinted[pick])
						++stats.hint_taken;
				}
			}
			step.chosen = pick;
			run.steps.push_back(std::move(step));
		}

		duel.SetResponse(choices[pick].response);
		path.push_back(choices[pick].response);
	}
	// Sortie par le HAUT : plafond de decisions (C2).
	++stats.edges_skipped;
}

void AdaptRun(NrpaPolicy& pol, NrpaResidual* res, const NrpaRun& run,
			  float alpha, float bias_known, float hint_bias, float shrink,
			  float temp) {
	const double inv_t = 1.0 / (temp > 1e-3f ? temp : 1e-3f);
	const bool two_level = res && shrink >= 0.0f;
	std::vector<double> p;
	for(const PolicyStep& s : run.steps) {
		p.resize(s.keys.size());
		double mx = -1e300;
		for(size_t i = 0; i < s.keys.size(); ++i) {
			double w = EffectiveWeight(pol, res, s.keys[i], s.ctx, shrink);
			p[i] = (w + (s.known[i] ? bias_known : 0.0f) +
					(i < s.hinted.size() && s.hinted[i] ? hint_bias : 0.0f)) *
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
		(*res)[CtxKey(s.keys[s.chosen], s.ctx)].w += alpha;
		for(size_t i = 0; i < s.keys.size(); ++i) {
			CtxWeight& c = (*res)[CtxKey(s.keys[i], s.ctx)];
			c.w -= static_cast<float>(alpha * p[i] / sum);
			++c.n;
		}
	}
}

void AdaptCorpus(NrpaPolicy& pol, NrpaResidual* res,
				 const std::vector<NrpaRun>& runs, uint32_t passes, float alpha,
				 float bias_known, float hint_bias, float shrink, float temp) {
	for(uint32_t pass = 0; pass < passes; ++pass)
		for(const NrpaRun& r : runs)
			AdaptRun(pol, res, r, alpha, bias_known, hint_bias, shrink, temp);
}

void Search::Adapt(Policy& pol, const NrpaRun& best) {
	AdaptRun(pol, &ctx_weights, best, cfg.nrpa_alpha, cfg.nrpa_bias_known,
			 cfg.hint_bias, cfg.ctx_shrink, cfg.nrpa_temp);
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
	};
	std::vector<LNode> nodes;
	nodes.push_back({ -1, 0, 0.0f, {}, 0.0f, 1.0, 0xffffu,
					  std::numeric_limits<double>::infinity(),
					  std::numeric_limits<double>::infinity() });
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
	std::priority_queue<QE, std::vector<QE>, std::greater<QE>> pq;
	pq.push({ 0.0, 0 });

	std::vector<uint32_t> chain;
	std::vector<double> logit;
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
							 actions, resolved))
					return Adv::Goal;
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

		// Le parent est-il dans la pile de plongee ?
		int64_t at = -1;
		for(size_t i = dive.size(); i-- > 0;)
			if(static_cast<int32_t>(dive[i].node) == parent) {
				at = static_cast<int64_t>(i);
				break;
			}

		bool dead = false;
		if(parent >= 0 && at < 0) {
			// Parent hors pile : premier retour a la racine, rejeu du chemin
			// de decisions (les coups forces se re-derivent).
			while(!dive.empty()) {
				arena.Pop();
				dive.pop_back();
			}
			chain.clear();
			for(int32_t i = static_cast<int32_t>(idx); i > 0;
				i = nodes[i].parent)
				chain.push_back(static_cast<uint32_t>(i));
			std::reverse(chain.begin(), chain.end());
			arena.Restore();
			path.clear();
			actions = 0;
			turns = cfg.initial_turns;
			summons = cfg.initial_summons;
			resolved = cfg.initial_resolved;
			size_t ci = 0;
			for(;;) {
				Adv a = advance(ci == chain.size());
				if(a != Adv::Branch) {
					dead = true;   // mort ou but : pas d'enfants
					break;
				}
				if(ci == chain.size())
					break;
				const std::vector<uint8_t>& r = nodes[chain[ci]].response;
				duel.SetResponse(r);
				path.push_back(r);
				++ci;
			}
		} else {
			// Enfant ou frere : depiler jusqu'au parent (chaque Pop restaure
			// et fusionne ses pages sales), restaurer son etat, jouer la
			// reponse du noeud. `at < 0` : le parent est la racine elle-meme.
			while(static_cast<int64_t>(dive.size()) > at + 1) {
				arena.Pop();
				dive.pop_back();
			}
			arena.Restore();
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
			if(idx != 0) {
				duel.SetResponse(nodes[idx].response);
				path.push_back(nodes[idx].response);
			}
			dead = advance(true) != Adv::Branch;
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
		const float nlogpi = nodes[idx].logpi;
		if(ndepth >= cfg.max_decisions) {
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
		double sum = 0;
		for(double& x : logit) {
			x = std::exp(x - mx);
			sum += x;
		}
		for(size_t i = 0; i < nc; ++i) {
			LNode child;
			child.parent = static_cast<int32_t>(idx);
			child.depth = ndepth + 1;
			child.logpi = nlogpi + static_cast<float>(std::log(
				(std::max)(logit[i] / sum, 1e-30)));
			child.response = ro_choices[i].response;
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

size_t LiftPolicyRun(Duel& duel, Arena& arena, const Replay& yrp,
					 int target_player, size_t stop_after, const EnumOptions& eo,
					 const std::unordered_map<uint64_t, size_t>& repertoire,
					 const BoardKey& target, NrpaRun& out) {
	size_t unknown = 0, ri = 0;
	uint8_t ptype = 0;
	std::vector<uint8_t> payload;
	int player = -1;
	BoardKey here;
	out.score = 0;
	out.steps.clear();

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
					out.steps.push_back(std::move(step));
				} else {
					++unknown;
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
				double w = EffectiveWeight(pol, res, s.keys[i], s.ctx, shrink);
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
				double w = EffectiveWeight(pol, res, s.keys[i], s.ctx, shrink);
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
			uint64_t sig = use_ctx ? (s.ctx + 1) * 0x9e3779b97f4a7c15ull : 0;
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
