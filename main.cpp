// combosolver — jalon 0 : rejeu instrumente et validation de l'arene.
//
// Reproduit fidelement la ligne jouee dans un .yrpX, mesure son cout et le
// branchement offert par le core a chaque decision, capture le board cible,
// puis verifie que l'instantane memoire restaure un etat rigoureusement
// identique. Sans rejeu fidele, le board cible est faux ; sans restauration
// fidele, toute la recherche l'est aussi.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "arena.h"
#include "assets.h"
#include "duel.h"
#include "enumerate.h"
#include "prompt.h"
#include "replay.h"
#include "search.h"

using namespace solver;

namespace {

using Clock = std::chrono::steady_clock;

double MsSince(Clock::time_point t0) {
	return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
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
};

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
		"  --outdir <dir>     ou ecrire les replays produits (defaut solutions/)\n"
		"  --solve            recherche guidee vers le board cible\n"
		"  --solve-ms <ms>    budget temps de la recherche (defaut 120000)\n"
		"  --threads <n>      workers de recherche (defaut : tous les coeurs)\n"
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
	return !o.replay.empty();
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
	Board target_self, target_oppo;
	size_t target_at = 0;
	// Position de depart, capturee au tout premier point de decision : c'est
	// elle qui dit si un deck a seulement de quoi commencer.
	bool have_start = false;
	Board start_self;
	uint64_t fingerprint_at_target = 0;
	uint64_t fingerprint_final = 0;
	double ms = 0;
	// Pages salies entre deux decisions consecutives. C'est LA granularite qui
	// compte : le solveur branche a chaque decision, pas a chaque action, donc
	// c'est a ce rythme qu'il paiera un instantane.
	std::vector<size_t> dirty_per_decision;
	double ms_write_watch = 0;   // cout cumule des appels GetWriteWatch
	size_t write_watch_calls = 0;
};

// Deroule les reponses enregistrees. `instrument` active la collecte complete ;
// une seconde passe de verification n'en a pas besoin.
LineResult RunLine(Duel& duel, const Replay& yrp, const Options& opt, bool instrument) {
	LineResult r;
	auto t0 = Clock::now();
	Arena* arena = duel.GetArena();
	uint32_t phase = 0;
	bool first_idle_seen = false;

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
				if(r.turns == 2 && !r.have_target) {
					r.target_self = Snapshot(duel, uint8_t(opt.target_player));
					r.target_oppo = Snapshot(duel, uint8_t(1 - opt.target_player));
					r.target_at = r.responses_used;
					r.fingerprint_at_target = Fingerprint(duel);
					r.have_target = true;
				}
				break;
			case MSG_NEW_PHASE:
				if(m.size >= 2) { uint16_t p = 0; std::memcpy(&p, m.data, 2); phase = p; }
				break;
			case MSG_SUMMONING:     ++r.summon; break;
			case MSG_SPSUMMONING:   ++r.spsummon; break;
			case MSG_FLIPSUMMONING: ++r.flipsummon; break;
			case MSG_CHAINING:      ++r.chaining; break;
			case MSG_RETRY:         ++r.retries; break;
			default: break;
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
			duel.SetResponse(yrp.responses[r.responses_used]);
			++r.responses_used;
		} else if(status == OCG_DUEL_STATUS_END) {
			break;
		} else if(status != OCG_DUEL_STATUS_CONTINUE) {
			std::printf("\n!! statut de duel inattendu : %d\n", status);
			break;
		}
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
		std::printf("\n--- board cible (fin du tour du joueur %d) ---\n",
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
size_t Advance(Duel& duel, const Replay& yrp, size_t from, size_t n,
			   uint32_t* actions = nullptr) {
	size_t used = 0;
	while(used < n) {
		int status = duel.Process();
		for(const Message& m : duel.Messages()) {
			if(actions && (m.type == MSG_SUMMONING || m.type == MSG_SPSUMMONING ||
						   m.type == MSG_FLIPSUMMONING || m.type == MSG_CHAINING))
				++*actions;
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
size_t WriteSolutions(const std::vector<Solution>& sols, const Replay& start_yrp,
					  const BoardKey& target, const Options& opt, CardDB& db,
					  ScriptProvider& scripts, const std::string& outdir);

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
	eo.max_subsets = 24;

	// Comparer les octets serait trop strict : EDOPro encode ses selections en
	// bitset (type 3), l'enumerateur en liste d'index (type 2), et la
	// deduplication par code choisit un representant qui n'est pas forcement
	// celui qu'a designe le joueur. Le seul critere qui a du sens est l'ETAT
	// ATTEINT : une reponse enumeree couvre la reponse enregistree si elle mene
	// exactement au meme etat. L'arene rend ce test abordable.
	auto advance_one = [&](const std::vector<uint8_t>& resp) -> uint64_t {
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
		return Fingerprint(duel);
	};

	struct Cov { int total = 0, covered = 0, empty = 0; };
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
		arena.Restore();
		bool found = false;
		for(const auto& ch : choices) {
			if(advance_one(ch.response) == want && want != 0) {
				found = true;
				arena.Restore();
				break;
			}
			arena.Restore();
		}
		arena.Pop();

		if(found)
			++c.covered;
		else if(misses.size() < 8) {
			misses.push_back(std::string(PromptName(ptype)) + " #" +
							 std::to_string(ri) + " joueur " + std::to_string(player) +
							 " : aucune des " + std::to_string(choices.size()) +
							 " propositions n'atteint l'etat enregistre");
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

	int total = 0, covered = 0;
	std::printf("  %-24s %8s %8s %8s\n", "type", "n", "couvert", "taux");
	for(const auto& [type, c] : cov) {
		std::printf("  %-24s %8d %8d %7.0f%%%s\n", PromptName(type), c.total,
					c.covered, c.total ? 100.0 * c.covered / c.total : 0.0,
					c.empty ? "   (enumeration vide)" : "");
		total += c.total;
		covered += c.covered;
	}
	std::printf("  %-24s %8d %8d %7.0f%%\n", "TOTAL", total, covered,
				total ? 100.0 * covered / total : 0.0);
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
// ligne de reference.
void RunSolve(Duel& duel, const Replay& yrp, const Options& opt, Arena& arena,
			  const LineResult& ref, CardDB& db, ScriptProvider& scripts) {
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
			return;
	}

	std::printf("  cible          : %zu cartes\n", target.entries.size());
	std::printf("  reference      : %u actions, %zu decisions, %u cartes brulees"
				"  (mesure a l'instant du board)\n",
				ref_actions, ref_decisions, ref_burned);

	SearchConfig cfg;
	cfg.target_player = opt.target_player;
	// Bornes issues de la ligne de reference : on ne cherche que des lignes qui
	// ne sont pires ni en actions ni en decisions (section 3.2).
	cfg.max_decisions = static_cast<uint32_t>(ref_decisions);
	cfg.max_actions = ref_actions;
	cfg.time_limit_ms = opt.solve_ms;
	cfg.max_nodes = 50000000;
	cfg.max_solutions = 16;
	cfg.enumeration.dedup_by_code = true;
	cfg.enumeration.max_subsets = 24;

	// Approfondissement progressif du nombre d'ecarts. A zero ecart la
	// recherche rejoue la reference, donc elle trouve toujours au moins une
	// solution : "aucune solution" redevient un signal de defaut, pas un
	// resultat possible.
	std::vector<Solution> sols;
	double spent = 0;
	unsigned threads = opt.threads ? opt.threads
								   : (std::max)(1u, std::thread::hardware_concurrency());
	std::printf("  workers        : %u\n", threads);
	std::printf("\n  %-8s %10s %12s %11s %9s\n", "ecarts", "solutions", "etats",
				"transpos.", "duree");
	uint32_t reached = 0;
	for(uint32_t k = 0; k <= 12 && spent < opt.solve_ms; ++k) {
		reached = k;
		double budget = opt.solve_ms - spent;
		auto t0 = Clock::now();

		// k = 0 suit un chemin unique : rien a paralleliser, et c'est le
		// controle qui doit retrouver la reference.
		unsigned n = (k == 0) ? 1u : threads;
		// Un jeton par point de deviation possible le long de l'echine.
		std::vector<std::atomic<uint32_t>> claims(ref_decisions + 1);
		for(auto& c : claims)
			c.store(0, std::memory_order_relaxed);
		std::mutex merge;
		std::vector<Solution> found;
		uint64_t nodes = 0, transpos = 0;
		bool timed_out = false;

		auto worker = [&](unsigned id) {
			// Chaque worker a SA propre arene et SON propre duel : les
			// instantanes ne circulent pas entre threads (bases distinctes).
			Arena local_arena;
			std::string err;
			if(!local_arena.Init(opt.arena_mb << 20, 0, err))
				return;
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
					if(n > 1) {
						// A un seul ecart il n'y a pas de second niveau : on
						// partage le premier, faute de mieux.
						wcfg.claim_level = (k <= 1) ? 0u : 1u;
						wcfg.claims = claims.data();
						wcfg.claims_size = claims.size();
					}
					Search s(local, local_arena, yrp, wcfg);
					s.RunRepair(target, k);
					std::lock_guard<std::mutex> lock(merge);
					for(const auto& x : s.Solutions())
						found.push_back(x);
					nodes += s.Stats().nodes;
					transpos += s.Stats().transpositions;
					timed_out |= s.Stats().hit_time_limit;
				}
			}
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

		double ms = MsSince(t0);
		spent += ms;
		std::printf("  %-8u %10zu %12llu %11llu %8.1f s%s\n", k, found.size(),
					(unsigned long long)nodes, (unsigned long long)transpos,
					ms / 1000.0, timed_out ? "  (budget epuise)" : "");
		for(const auto& x : found)
			sols.push_back(x);
		if(k == 0 && found.empty()) {
			std::printf("\n  A zero ecart la reference doit etre retrouvee. "
						"Elle ne l'est pas : defaut du moteur.\n");
			return;
		}
	}
	arena.Restore();

	if(sols.empty()) {
		std::printf("\n  AUCUNE solution atteinte.\n");
		return;
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
	WriteSolutions(sols, yrp, target, opt, db, scripts, opt.outdir);
	if(!better)
		std::printf("\n  Aucune ligne strictement meilleure trouvee.\n"
					"  La reference resiste a %u deviation(s) simultanee(s).\n",
					reached);
	(void)ref;
	(void)db;
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
			return;
		{
			Duel d(db, scripts, &a);
			if(d.Create(yrp.seed, yrp.duel_flags, yrp.start_lp, yrp.start_hand,
						yrp.draw_count, err) && d.Setup(yrp, err)) {
				while(d.Process() == OCG_DUEL_STATUS_CONTINUE) {}
				for(const auto& c : d.Query(con, LOCATION_HAND,
											QUERY_CODE | QUERY_ALIAS))
					if(c.present)
						out.push_back(db.Canonical(c.Code()));
			}
		}
		a.Shutdown();
	}).join();
	return out;
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
					  ScriptProvider& scripts, const std::string& outdir) {
	std::error_code ec;
	std::filesystem::create_directories(outdir, ec);
	size_t written = 0, rejected = 0;
	const auto con = static_cast<uint8_t>(opt.target_player);

	// Thread dedie : une arene ne s'initialise jamais sur un thread qui en
	// possede deja une.
	std::thread([&] {
		Arena a;
		std::string err;
		if(!a.Init(opt.arena_mb << 20, 0, err))
			return;
		{
			Duel d(db, scripts, &a);
			if(!d.Create(start_yrp.seed, start_yrp.duel_flags, start_yrp.start_lp,
						 start_yrp.start_hand, start_yrp.draw_count, err) ||
			   !d.Setup(start_yrp, err))
				return;
			if(opt.stop_gc)
				d.SetLuaGc(false);
			a.Push();
			for(size_t i = 0; i < sols.size() && i < 16; ++i) {
				size_t used = 0;
				bool retry = false;
				while(used < sols[i].responses.size() && !retry) {
					int status = d.Process();
					for(const Message& m : d.Messages())
						if(m.type == MSG_RETRY)
							retry = true;
					if(status == OCG_DUEL_STATUS_AWAITING)
						d.SetResponse(sols[i].responses[used++]);
					else if(status != OCG_DUEL_STATUS_CONTINUE)
						break;
				}
				while(d.Process() == OCG_DUEL_STATUS_CONTINUE) {}
				const bool ok = !retry &&
								ComputeBoardKey(d, con) == target;
				if(ok) {
					char name[64];
					std::snprintf(name, sizeof(name),
								  "solution_%02zu_b%u_a%u.yrp", i,
								  sols[i].burned, sols[i].actions);
					std::string path = outdir + "/" + name;
					std::string werr;
					if(WriteYrp1(path, start_yrp, sols[i].responses, werr))
						++written;
					else
						std::printf("  !! %s\n", werr.c_str());
				} else {
					++rejected;
				}
				a.Restore();
			}
			a.Pop();
		}
		a.Shutdown();
	}).join();

	std::printf("\n--- sortie ---\n");
	std::printf("  %zu replay(s) ecrits dans %s\n", written, outdir.c_str());
	if(rejected)
		std::printf("  %zu rejetee(s) : rejouees depuis zero, elles ne refont "
					"pas le board\n", rejected);
	return written;
}

// TRANSPLANTATION — refaire le board de reference depuis un AUTRE deck.
//
// Le probleme n'est plus d'ameliorer une ligne connue mais d'en reconstruire
// une : les reponses enregistrees ne designent rien dans un duel dont ni le
// deck, ni la main, ni la graine ne coincident. Ce qu'on transporte, c'est
// l'INTENTION de la ligne (LiftPlan), et on s'en sert comme ordre de visite.
void RunTransplantSolve(Duel& duel, const Replay& ref_yrp, const Replay& start_yrp,
						const Options& opt, Arena& arena, const LineResult& ref,
						CardDB& db, ScriptProvider& scripts) {
	std::printf("\n=== transplantation du combo sur un autre deck ===\n");
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
		eo.max_subsets = 24;
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
	if(plan.empty()) {
		std::printf("\n  Plan vide : rien a transplanter.\n");
		return;
	}

	// --- 4. Recherche, par approfondissement progressif du nombre d'ecarts.
	SearchConfig cfg;
	cfg.target_player = opt.target_player;
	// Le plan compte 273 etapes ; un autre deck en demandera davantage pour
	// arriver au meme endroit. On laisse de la marge, sans quoi la borne
	// couperait avant le board.
	cfg.max_decisions = static_cast<uint32_t>(ref_decisions * 3 / 2 + 32);
	cfg.max_actions = 0;         // aucune borne : on cherche d'abord A atteindre
	cfg.max_nodes = 50000000;
	cfg.max_solutions = 16;
	cfg.enumeration.dedup_by_code = true;
	cfg.enumeration.max_subsets = 24;
	cfg.enumeration.db = &db;
	cfg.plan_window = 32;

	unsigned threads = opt.threads ? opt.threads
								   : (std::max)(1u, std::thread::hardware_concurrency());
	std::vector<Solution> sols;
	uint32_t best_overlap = 0, best_monsters = 0;
	std::vector<uint32_t> best_board;

	// --- 4a. Sonde gloutonne. La recherche a ecarts bornes ne descend qu'aussi
	// profond que son budget d'ecarts ; quand le plan ne s'applique pas des
	// l'ouverture, cela plafonne a une dizaine de decisions alors que le board
	// en demande des centaines. La descente guidee, elle, va au fond : elle dit
	// jusqu'ou ce deck sait aller, ce qu'aucun echec de LDS ne revele.
	{
		std::printf("\n--- sonde : jusqu'ou ce deck va-t-il depuis cette main ? ---\n");
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
				   probe.Setup(start_yrp, err)) {
					if(opt.stop_gc)
						probe.SetLuaGc(false);
					SearchConfig pcfg = cfg;
					pcfg.time_limit_ms = (std::min)(opt.solve_ms / 3.0, 60000.0);
					Search s(probe, probe_arena, start_yrp, pcfg);
					s.RunGuided(target);
					const SearchStats& st = s.Stats();
					std::printf("  %llu etats, %.1f s : au mieux %u des %zu cartes "
								"cibles, %u monstre(s)\n",
								(unsigned long long)st.nodes, st.ms / 1000.0,
								st.best_overlap, target.codes.size(),
								st.best_monsters);
					if(st.best_overlap > best_overlap)
						best_board = st.best_board;
					best_overlap = (std::max)(best_overlap, st.best_overlap);
					best_monsters = (std::max)(best_monsters, st.best_monsters);
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
	}

	// --- 4b. Tirages gloutons. C'est la passe qui a une chance d'aller au bout :
	// elle descend jusqu'a la fin du tour a chaque essai, la ou les deux autres
	// s'arretent a quelques dizaines de decisions.
	{
		std::printf("\n--- tirages gloutons guides par le repertoire ---\n");
		double budget = (std::min)(opt.solve_ms / 2.0, 300000.0);
		std::mutex merge;
		uint64_t nodes = 0;
		uint32_t rollouts_done = 0;
		auto t0 = Clock::now();

		auto worker = [&](unsigned id) {
			Arena la;
			std::string err;
			if(!la.Init(opt.arena_mb << 20, 0, err))
				return;
			{
				Duel local(db, scripts, &la);
				if(local.Create(start_yrp.seed, start_yrp.duel_flags,
								start_yrp.start_lp, start_yrp.start_hand,
								start_yrp.draw_count, err) &&
				   local.Setup(start_yrp, err)) {
					if(opt.stop_gc)
						local.SetLuaGc(false);
					SearchConfig wcfg = cfg;
					wcfg.time_limit_ms = budget;
					Search s(local, la, start_yrp, wcfg);
					// Graine distincte par worker : sans cela les seize tirent
					// exactement la meme sequence de lignes.
					s.RunRollouts(target, plan, 1000000,
								  0x243f6a8885a308d3ull + id * 0x100000001b3ull);
					std::lock_guard<std::mutex> lock(merge);
					for(const auto& x : s.Solutions())
						sols.push_back(x);
					nodes += s.Stats().nodes;
					if(s.Stats().best_overlap > best_overlap)
						best_board = s.Stats().best_board;
					best_overlap = (std::max)(best_overlap, s.Stats().best_overlap);
					best_monsters = (std::max)(best_monsters, s.Stats().best_monsters);
					rollouts_done += 1;
				}
			}
			la.Shutdown();
		};

		std::vector<std::thread> pool;
		for(unsigned i = 0; i < threads; ++i)
			pool.emplace_back(worker, i);
		for(auto& t : pool)
			t.join();
		std::printf("  %llu etats en %.1f s : au mieux %u des %zu cartes cibles, "
					"%u monstre(s)\n", (unsigned long long)nodes,
					MsSince(t0) / 1000.0, best_overlap, target.codes.size(),
					best_monsters);
		if(!sols.empty())
			std::printf("  %zu ligne(s) atteignant le board.\n", sols.size());
		(void)rollouts_done;
	}

	if(!sols.empty()) {
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
		WriteSolutions(sols, start_yrp, target, opt, db, scripts, opt.outdir);
		arena.Restore();
		return;
	}

	std::printf("\n--- recherche a ecarts bornes autour du plan ---\n");
	std::printf("  workers        : %u,  profondeur max %u decisions\n", threads,
				cfg.max_decisions);
	std::printf("\n  %-8s %10s %12s %11s %9s\n", "ecarts", "solutions", "etats",
				"transpos.", "duree");

	double spent = 0;
	uint32_t reached = 0;
	for(uint32_t k = 0; k <= 12 && spent < opt.solve_ms && sols.empty(); ++k) {
		reached = k;
		double budget = opt.solve_ms - spent;
		auto t0 = Clock::now();
		unsigned n = (k == 0) ? 1u : threads;
		std::vector<std::atomic<uint32_t>> claims(plan.size() + 1);
		for(auto& c : claims)
			c.store(0, std::memory_order_relaxed);
		std::mutex merge;
		std::vector<Solution> found;
		uint64_t nodes = 0, transpos = 0;
		bool timed_out = false;

		auto worker = [&](unsigned) {
			// Chaque worker a son arene et son duel — et ce duel est monte sur le
			// replay de DEPART, pas sur la reference.
			// (best_overlap / best_monsters remontent par `merge`.)
			Arena local_arena;
			std::string err;
			if(!local_arena.Init(opt.arena_mb << 20, 0, err))
				return;
			{
				Duel local(db, scripts, &local_arena);
				if(local.Create(start_yrp.seed, start_yrp.duel_flags,
								start_yrp.start_lp, start_yrp.start_hand,
								start_yrp.draw_count, err) &&
				   local.Setup(start_yrp, err)) {
					if(opt.stop_gc)
						local.SetLuaGc(false);
					SearchConfig wcfg = cfg;
					wcfg.time_limit_ms = budget;
					wcfg.trace = opt.verbose && k == 0;
					if(n > 1) {
						wcfg.claim_level = (k <= 1) ? 0u : 1u;
						wcfg.claims = claims.data();
						wcfg.claims_size = claims.size();
					}
					Search s(local, local_arena, start_yrp, wcfg);
					s.RunTransplant(target, plan, k);
					std::lock_guard<std::mutex> lock(merge);
					for(const auto& x : s.Solutions())
						found.push_back(x);
					nodes += s.Stats().nodes;
					transpos += s.Stats().transpositions;
					timed_out |= s.Stats().hit_time_limit;
					if(s.Stats().best_overlap > best_overlap)
						best_board = s.Stats().best_board;
					best_overlap = (std::max)(best_overlap, s.Stats().best_overlap);
					best_monsters = (std::max)(best_monsters, s.Stats().best_monsters);
				} else {
					std::lock_guard<std::mutex> lock(merge);
					std::printf("  !! duel de depart non initialisable : %s\n",
								err.c_str());
				}
			}
			local_arena.Shutdown();
		};

		std::vector<std::thread> pool;
		for(unsigned i = 0; i < n; ++i)
			pool.emplace_back(worker, i);
		for(auto& t : pool)
			t.join();

		double ms = MsSince(t0);
		spent += ms;
		std::printf("  %-8u %10zu %12llu %11llu %8.1f s   %u/%zu %u mon.%s\n", k,
					found.size(), (unsigned long long)nodes,
					(unsigned long long)transpos, ms / 1000.0, best_overlap,
					target.codes.size(), best_monsters,
					timed_out ? "  (budget epuise)" : "");
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
	WriteSolutions(sols, start_yrp, target, opt, db, scripts, opt.outdir);
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

	std::printf("  %-6s %12s %12s %12s %11s %9s %8s\n", "prof.", "etats",
				"transpos.", "impasses", "solutions", "duree", "statut");
	uint64_t prev = 0;
	for(uint32_t depth = 2; depth <= opt.growth_max; depth += 2) {
		SearchConfig cfg;
		cfg.target_player = opt.target_player;
		cfg.max_decisions = depth;
		cfg.time_limit_ms = opt.growth_ms;
		cfg.max_nodes = 5000000;
		cfg.enumeration.dedup_by_code = true;
		cfg.enumeration.max_subsets = 24;

		Search search(duel, arena, yrp, cfg);
		search.Run(target);
		arena.Restore();

		const SearchStats& s = search.Stats();
		const char* status = s.hit_time_limit ? "temps"
							 : s.hit_node_limit ? "noeuds" : "epuise";
		std::printf("  %-6u %12llu %12llu %12llu %11zu %8.0f ms %8s",
					depth, (unsigned long long)s.nodes,
					(unsigned long long)s.transpositions,
					(unsigned long long)s.dead_ends,
					search.Solutions().size(), s.ms, status);
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
			std::printf("\n  terminaux : %llu   impasses : %llu\n",
						(unsigned long long)s.terminals,
						(unsigned long long)s.dead_ends);
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
	std::string error;

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

	ScriptProvider scripts;
	scripts.Init(opt.workdir, opt.scriptdirs);
	std::printf("  dossiers scripts  : %zu%s\n", scripts.Dirs().size(),
				opt.scriptdirs.empty() ? "" : "  (override)");

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
		if(!duel.Setup(*yrp, error)) {
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

		LineResult first = RunLine(duel, *yrp, opt, true);
		ReportLine(first, *yrp, db, opt);

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
			std::printf("  sorties d'arene   : %zu  %s\n", st.host_fallbacks,
						st.host_fallbacks ? "<-- ANOMALIE : etat hors instantane"
										  : "(aucune : tout l'etat est capture)");

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
			if(ok && opt.growth)
				RunGrowthMeasurement(duel, *yrp, opt, *arena_ptr, first);
			if(ok && opt.solve) {
				if(start_yrp)
					RunTransplantSolve(duel, *yrp, *start_yrp, opt, *arena_ptr,
									   first, db, scripts);
				else
					RunSolve(duel, *yrp, opt, *arena_ptr, first, db, scripts);
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
		if(!duel.Errors().empty()) {
			std::printf("\n  erreurs du core (%zu) :\n", duel.Errors().size());
			for(size_t i = 0; i < duel.Errors().size() && i < 6; ++i)
				std::printf("      %s\n", duel.Errors()[i].c_str());
		}
	}
	arena.Shutdown();
	return exit_code;
}
