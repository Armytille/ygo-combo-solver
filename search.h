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
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "duel.h"
#include "enumerate.h"
#include "prompt.h"
#include "replay.h"

namespace solver {

// Board cible au sens du critere d'equivalence retenu : memes cartes par TYPE
// de zone, memes materiaux, memes compteurs, meme FACE (recto/verso). La
// colonne exacte est ignoree, et la position de combat ATK/DEF aussi
// (arbitrage du joueur, session 4 : deux boards qui ne different que par une
// position de combat sont le meme board). Le digest d'ETAT garde, lui, la
// position complete.
struct BoardKey {
	uint64_t hash = 0;
	std::vector<uint64_t> entries;   // triees, pour une comparaison exacte
	// Codes seuls, tries. Beaucoup plus grossier que `entries`, mais c'est
	// justement ce qu'il faut pour guider : une carte posee compte des qu'elle
	// est la, sans attendre d'avoir ses materiaux et sa position finale.
	std::vector<uint32_t> codes;
	// Monstres presents, capture au passage : evite une requete de zone
	// supplementaire a chaque evaluation d'heuristique (point chaud mesure).
	uint32_t mzone_count = 0;
	bool operator==(const BoardKey& o) const { return entries == o.entries; }
};

BoardKey ComputeBoardKey(Duel& duel, uint8_t con);
// Variante reutilisant `out` : deux requetes de zone par decision, des dizaines
// de millions de fois par run — l'allocation par appel etait un point chaud.
void ComputeBoardKeyInto(Duel& duel, uint8_t con, BoardKey& out);

// Construit une BoardKey depuis des listes de cartes EXPLICITES — l'edition du
// board cible (retirer Hot Red, exiger Naturia Beast) passe par la : on part
// des cartes capturees au board de reference, on retire, on ajoute, et on
// recompose la cle exactement comme ComputeBoardKey l'aurait fait.
BoardKey MakeBoardKey(const std::vector<QueriedCard>& mzone,
					  const std::vector<QueriedCard>& szone, const CardDB& db);

// Cardinal de l'intersection des codes tries : combien de cartes du board
// cible sont posees. C'est le "nombre de sous-buts atteints" qui serialise le
// but conjonctif.
uint32_t CommonCodes(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b);

// --- Elagage par nouveaute (Iterated Width) ------------------------------
//
// La table de transposition ne fusionne que les etats IDENTIQUES ; or deux
// lignes qui different d'une carte au cimetiere sont distinctes et pourtant
// sans interet distinct. L'elagage par nouveaute renverse le critere : un etat
// n'est retenu que s'il rend vrai au moins un ATOME — un fait (zone, carte,
// occurrence) du joueur cible — inedit, ou atteint plus tot que jamais. Le
// cout devient exponentiel dans la LARGEUR du probleme, pas dans la taille de
// l'espace (Lipovetzky & Geffner ; Rollout-IW, arXiv:1801.03354).

// Atomes d'un etat : entrees du terrain (fines et par code), contenu des zones
// cachees avec occurrence, compte du deck. `partition` est melange a chaque
// atome : partitionner par sous-but atteint rouvre la table a chaque carte
// cible posee — c'est la serialisation du but (Serialized IW / BFWS).
void CollectAtoms(Duel& duel, uint8_t con, const BoardKey& here,
				  uint32_t partition, std::vector<uint64_t>& out);

// --- Garde de board (fenetres Nibiru) ------------------------------------
//
// Une clause est une conjonction de conditions (carte presente dans une des
// zones du masque). La garde tient si AU MOINS UNE clause tient entierement.
// Exemple : "Crystal Wing@terrain" OU "Zalen@terrain + Junk Signal@main".
struct GuardAtom {
	uint32_t code = 0;    // canonique
	uint32_t zones = 0;   // masque LOCATION_*
};
using GuardClause = std::vector<GuardAtom>;

// La garde tient-elle dans l'etat courant ? `field_codes` : codes tries des
// cartes MZONE+SZONE du joueur (ceux de BoardKey — la zone terrain d'ocgcore
// est le slot 5 de SZONE, le terrain y est donc couvert). Les autres zones ne
// sont interrogees que si une clause les mentionne.
bool GuardHolds(Duel& duel, uint8_t con, const std::vector<GuardClause>& clauses,
				const std::vector<uint32_t>& field_codes);

// Minimum de resolutions d'effet (--resolve "carte[@zone][:n]"). `zones`
// restreint la zone d'ACTIVATION : l'Omega qui rippe la main s'active du
// TERRAIN — compter son effet de cimetiere etait un faux positif, paye sur le
// jugement d'une approche. 0 = toutes zones (comportement d'avant).
struct ResolveReq {
	uint32_t code = 0;        // canonique
	uint32_t min_count = 1;
	uint32_t zones = 0;       // masque LOCATION_ de la zone d'activation
	// --summon-min : compter les INVOCATIONS (MSG_SUMMONING/SPSUMMONING) de la
	// carte au lieu de ses activations. Meme mecanique de bout en bout (gate
	// au but, gradient, biais, histogramme). Le cas d'usage mesure : Junk
	// Meister s'invoque par son propre effet (revele Stardust/Accel/3e
	// synchro) — sa POSE est l'evenement rare qui verrouille Naturia Beast,
	// pas une activation.
	bool on_summon = false;
};

// Zone d'ACTIVATION d'un MSG_CHAINING : triggering_location. Charge utile
// (processor.cpp:3700) : code u32, loc_info (controler u8, location u8,
// sequence u32, position u32), triggering_controler u8, triggering_location
// u8 a l'offset 15. 0 si le message est trop court.
inline uint32_t ChainingLocation(const uint8_t* data, uint32_t size) {
	return size >= 16 ? data[15] : 0u;
}

class NoveltyTable {
public:
	// Rend true si au moins un atome est inedit — ou, en mode non strict, vu
	// seulement plus profond — et met la table a jour.
	//
	// Deux regimes, choisis sur MESURE et non par gout :
	// - non strict (sensible a la profondeur) : refaire un fait en MOINS de
	//   decisions compte comme nouveau. C'est le regime sur pour la reparation
	//   meme-deck (il n'y perd aucune solution), mais un DFS qui visite les
	//   deviations profondes d'abord rend toute branche plus courte "nouvelle" :
	//   3 584 coupures sur 1 M d'etats a un ecart — inutile.
	// - strict : seul un fait JAMAIS vu compte. C'est l'elagage IW de la
	//   litterature, reserve aux passes ou l'on cherche du materiel neuf.
	bool Observe(const std::vector<uint64_t>& atoms, uint32_t depth,
				 bool strict = false) {
		bool novel = false;
		for(uint64_t a : atoms) {
			auto [it, fresh] = seen.emplace(a, depth);
			if(fresh) {
				novel = true;
			} else if(!strict && depth < it->second) {
				it->second = depth;
				novel = true;
			}
		}
		return novel;
	}
	// Variante sans mise a jour, pour compter sans consommer.
	bool WouldBeNovel(const std::vector<uint64_t>& atoms, uint32_t depth) const {
		for(uint64_t a : atoms) {
			auto it = seen.find(a);
			if(it == seen.end() || depth < it->second)
				return true;
		}
		return false;
	}
	void Clear() { seen.clear(); }
	size_t Size() const { return seen.size(); }

private:
	std::unordered_map<uint64_t, uint32_t> seen;   // atome -> profondeur min
};

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

// Releve la ligne de reference pour la REPARATION, indexee par reponse
// (yrp.responses, les deux joueurs) :
// - `digests` : digest de l'etat AVANT chaque reponse -> index. Pendant
//   DescendRepair, un etat dont le digest est celui d'un point PLUS LOIN de la
//   reference y reprend la ligne : le suffixe enregistre redevient lisible
//   apres une deviation qui converge.
// - `keys` : plan_key du coup joue a chaque index (0 si non identifie ou
//   decision adverse) — l'appariement de LiftPlan, ici par index de reponse.
//   C'est le repertoire FENETRE de la reparation : apres une premiere
//   deviation, rejouer un coup de la reference voisin du point courant est
//   gratuit, et une permutation locale (echanger les invocations #4/#5, ~17
//   decisions d'ecart mesure) coute UNE deviation au lieu d'une par decision.
// Le duel doit etre au depart ; il est laisse ou la ligne s'arrete.
void LiftRefLine(Duel& duel, Arena& arena, const Replay& yrp, int target_player,
				 size_t stop_after, const EnumOptions& eo,
				 std::unordered_map<uint64_t, size_t>& digests,
				 std::vector<uint64_t>& keys);

// --- politique NRPA : partage entre workers --------------------------------
//
// Un poids par code de coup (plan_key), echantillonnage softmax. Une decision
// d'un tirage sous politique est memorisee pour l'adaptation ; les biais
// (repertoire, indices) y sont conserves, car l'adaptation doit recalculer les
// MEMES probabilites que l'echantillonnage.
struct PolicyStep {
	std::vector<uint64_t> keys;   // plan_key de chaque choix legal
	std::vector<uint8_t> known;   // au repertoire ?
	std::vector<uint8_t> hinted;  // engage une carte --hint ?
	size_t chosen = 0;
	// CONTEXTE de la decision (chantier 5ter) : nombre de cartes du board
	// cible deja posees. Descripteur SEMANTIQUE (pas positionnel : deux lignes
	// ne posent pas les memes questions au meme indice, piege 21) et disponible
	// des deux cotes — au tirage comme au relevé d'une ligne de corpus.
	uint16_t ctx = 0;
};
struct NrpaRun {
	double score = -1;
	std::vector<PolicyStep> steps;
};

// Meilleure sequence GLOBALE, partagee entre les workers NRPA. Les redemarrages
// oubliaient les sous-lignes apprises (mesure : 6/8 avec les memes manquants
// run apres run) ; ici chaque redemarrage repart de la meilleure ligne connue
// de TOUS les workers, et un worker qui stagne l'adopte. Mutex basse frequence :
// une prise par iteration de niveau superieur, pas par tirage.
struct NrpaShared {
	std::mutex mu;
	NrpaRun best;
};

// Politique NRPA : un poids par code de coup (plan_key). Publique parce
// qu'elle SURVIT au run desormais : les poids appris par les tirages meurent
// avec le run alors qu'ils sont exactement le guide qu'il faut au finisseur —
// RunLevin les consomme, et les workers les fusionnent (moyenne des poids).
using NrpaPolicy = std::unordered_map<uint64_t, float>;

// --- politique a DEUX NIVEAUX (chantier 5ter, inspire de MCPS 2510.06381) ---
//
// Le plafond mesure en session 7 (accord du corpus 44 % -> 66 %, palier des la
// premiere passe, 70 % au mieux sur une ligne SEULE) n'est pas un defaut de
// signal : c'est la REPRESENTATION. Un poids par plan_key est aveugle a l'etat,
// or la meme identite semantique revient a des dizaines d'endroits d'une meme
// ligne avec des choix differents — aucun jeu de poids ne peut reproduire cela.
//
// Enrichir la cle du contexte, seul, echangerait un plafond contre une famine :
// chaque case contextuelle verrait une fraction des mises a jour. MCPS repond a
// exactement ce dilemme en COMBINANT plusieurs estimateurs d'un meme coup au
// lieu d'en choisir un, ponderes par leur evidence. On garde donc les DEUX
// niveaux :
//
//     w_eff(coup, ctx) = (1 - s) * w_global[coup] + s * w_ctx[coup, ctx]
//     s = n / (n + k)   ou n = nombre de mises a jour de la case contextuelle
//
// Limites : n = 0 -> w_global exactement (aucune penalite de fragmentation, la
// case neuve ne dit rien) ; n >> k -> w_ctx (dependance a l'etat pleine) ;
// k < 0 -> mecanisme ETEINT, comportement d'avant bit pour bit.
//
// Ecart assume avec MCPS : ses trois estimateurs sont des moyennes de recompense
// sur des ensembles de playouts qui se recouvrent, et il les pondere par les
// effectifs bruts (beta = n/N, variance minimale sous independance). Ici les
// deux niveaux sont EMBOITES — le global agrege tous les contextes, son
// effectif domine toujours — et une ponderation par effectifs bruts
// n'accorderait jamais la main au contextuel. D'ou la retenue par un k calibre,
// et non par le rapport des effectifs. C'est une combinaison convexe de deux
// logits de MEME echelle : elle ne change pas la temperature du softmax, donc
// l'A/B mesure la dependance a l'etat et rien d'autre.
struct CtxWeight {
	float w = 0;
	uint32_t n = 0;
};
using NrpaResidual = std::unordered_map<uint64_t, CtxWeight>;

// Le descripteur de CONTEXTE, calcule a l'identique au tirage et au relevé.
// Deux axes, tous deux semantiques et bon marche : combien de cartes du board
// cible sont posees (0 -> 8, le combo se construit) et combien de cartes
// restent en main (5 -> 0, les ressources se depensent). Le premier seul ne
// separe pas assez : mesure sur le corpus, 8 cases pour ~165 decisions par
// ligne, +4,3 points d'accord seulement. La main est l'axe orthogonal naturel
// — deux moments a meme board mais a main differente ne posent pas les memes
// questions.
inline uint16_t ContextKey(uint32_t placed, uint32_t hand) {
	if(placed > 15) placed = 15;
	if(hand > 15) hand = 15;
	return static_cast<uint16_t>(placed * 16u + hand);
}

inline uint64_t CtxKey(uint64_t key, uint16_t ctx) {
	return key ^ ((static_cast<uint64_t>(ctx) + 1) * 0x9e3779b97f4a7c15ull);
}

// Poids effectif d'un coup sous la politique a deux niveaux. `shrink` < 0
// eteint le niveau contextuel (la fonction rend alors pol[key] exactement).
inline float EffectiveWeight(const NrpaPolicy& pol, const NrpaResidual* res,
							 uint64_t key, uint16_t ctx, float shrink) {
	auto it = pol.find(key);
	const float wg = (it == pol.end()) ? 0.0f : it->second;
	if(!res || shrink < 0.0f)
		return wg;
	auto ic = res->find(CtxKey(key, ctx));
	if(ic == res->end())
		return wg;
	const float s = static_cast<float>(ic->second.n) /
					(static_cast<float>(ic->second.n) + shrink);
	return (1.0f - s) * wg + s * ic->second.w;
}

// REJEU D'ADAPTATION du corpus (chantier 5bis, arXiv:2401.10431) : releve une
// ligne de solution sous forme de SEQUENCE DE DECISIONS de politique — a chaque
// prompt multi-choix de notre joueur, l'ensemble des plan_key LEGAUX et l'indice
// de celui que la ligne a joue. C'est exactement ce que consomme Adapt(), le
// gradient NRPA standard.
//
// Ce que cela apporte de plus que le prior par POIDS (`--prior`, mesure NEUTRE
// session 6) : le prior donnait la meme prime a un coup PARTOUT ; l'adaptation
// est DISCRIMINATIVE — un coup du corpus ne monte pas dans l'absolu, il monte
// CONTRE les coups qui lui etaient opposes a cet endroit precis, et un coup du
// corpus systematiquement ecarte ailleurs redescend. C'est la difference entre
// « ces coups existent » et « a ce carrefour, la solution prenait celui-ci ».
//
// Le duel doit etre au depart, et c'est le duel de l'EN-TETE de la ligne
// (piege 21 : on ne rejoue jamais une ligne de corpus sur le duel de depart —
// seules les identites semantiques traversent). `repertoire` est l'index des
// coups de la reference : il sert a reproduire le biais `known` de
// l'echantillonnage, sans quoi Adapt() calculerait un gradient sous une
// distribution qui n'est pas celle des tirages. Renvoie le nombre d'etapes non
// identifiees (sautees : on ne sait pas quel choix la ligne a pris).
// `target` sert au CONTEXTE de chaque etape (cartes du board cible deja
// posees) : le meme descripteur que celui calcule au tirage.
size_t LiftPolicyRun(Duel& duel, Arena& arena, const Replay& yrp,
					 int target_player, size_t stop_after, const EnumOptions& eo,
					 const std::unordered_map<uint64_t, size_t>& repertoire,
					 const BoardKey& target, NrpaRun& out);

// Probabilite moyenne (log) que `pol` donne aux coups CHOISIS par les lignes
// du corpus, sous les memes biais que l'echantillonnage. C'est l'instrument
// qui dit si le rejeu d'adaptation a mordu : a politique vierge il vaut la
// moyenne des -log(nb de choix legaux) ; s'il ne monte pas apres les passes,
// le mecanisme est inerte et il est inutile de payer un run pour l'apprendre
// (piege 40).
// Rend la moyenne GEOMETRIQUE de p(coup joue) — exp de la log-vraisemblance
// moyenne. `argmax_frac`, si fourni, recoit la FRACTION d'etapes ou le coup du
// corpus est celui que la politique classe premier : c'est la seule des deux
// qui se compare au plafond de CorpusCoherence. Les deux ensemble separent
// « la politique se trompe partout un peu » de « elle a raison presque
// partout et s'effondre sur quelques etapes » — une moyenne geometrique est
// ecrasee par une poignee de p proches de zero, et lue seule elle ferait
// conclure a un echec la ou il n'y en a pas.
double CorpusAgreement(const NrpaPolicy& pol, const NrpaResidual* res,
					   const std::vector<NrpaRun>& runs, float bias_known,
					   float shrink, double* argmax_frac = nullptr);

// PREVISION DE COUT DE RECHERCHE, calculee sur des lignes DEJA RESOLUES.
//
// La garantie de Levin Tree Search borne le nombre d'expansions par d/pi(sol),
// ou pi est le PRODUIT des probabilites de la politique le long de la ligne.
// C'est exactement la grandeur mesuree en session 7bis (masse), et c'est donc
// elle qui gouverne le cout du finisseur — pas le classement.
//
// sqrt-LTS (arXiv:2412.05196) decompose implicitement la recherche en q
// sous-taches ancrees sur des INDICES (« un indice peut etre donne des qu'une
// sous-tache est resolue »). Notre code produit deja ces indices a chaque
// noeud : le nombre de cartes du board cible posees, qui est la composante
// haute de PolicyStep::ctx. Une recherche decomposee sur ces q points coute,
// au mieux, la SOMME des bornes par segment au lieu du produit global.
//
// Cette fonction calcule les deux, sur les lignes du corpus, avant d'ecrire la
// moindre ligne d'algorithme : elle rend log10 de la borne monolithique et
// log10 de la borne decomposee. L'ecart des deux EST le gain que sqrt-LTS peut
// rendre au mieux — le papier ajoute au-dessus un facteur lie a l'incertitude
// du rerooter, donc c'est un plafond, pas une promesse.
struct CostForecast {
	double log10_mono = 0;    // log10 de d/pi sur la ligne entiere
	double log10_decomp = 0;  // log10 de somme_i d_i/pi_i
	double segments = 0;      // q moyen (points d'indice + 1)
	double worst_seg_log10 = 0;   // le segment le plus cher, log10 de d_i/pi_i
	size_t lines = 0;
};
CostForecast ForecastSearchCost(const NrpaPolicy& pol, const NrpaResidual* res,
								const std::vector<NrpaRun>& runs,
								float bias_known, float shrink);

// PLAFOND de la famille de politiques, mesure sur le corpus lui-meme.
//
// Une politique de cette forme est une fonction du couple (contexte, ensemble
// des coups legaux) : deux etapes qui presentent le MEME ensemble de choix dans
// le MEME contexte sont indiscernables pour elle, quoi qu'on mette dans les
// poids. Si le corpus y joue des coups differents, l'ecart est IRREDUCTIBLE —
// aucune passe, aucun k, aucun enrichissement de poids ne le comblera.
//
// On groupe donc les etapes par (contexte, ensemble legal) et on rend la
// fraction d'etapes qui jouent le coup MAJORITAIRE de leur groupe : c'est
// exactement ce qu'atteindrait la meilleure politique deterministe de cette
// famille. Comparer ce plafond a l'accord obtenu dit s'il faut continuer a
// enrichir le contexte, ou si le corpus se contredit lui-meme.
// `use_ctx` a false ignore le contexte : la difference des deux plafonds
// chiffre ce que le descripteur apporte, independamment de l'apprentissage.
double CorpusCoherence(const std::vector<NrpaRun>& runs, bool use_ctx,
					   size_t* groups = nullptr);

// Un pas d'adaptation NRPA sur une sequence (le gradient de Cazenave : +alpha
// au coup joue, -alpha*p a chacun des legaux). Libre plutot que membre pour que
// le relevé du corpus et les workers appliquent EXACTEMENT la meme mise a jour
// — un instrument qui mesure autre chose que ce que le run subit ne mesure
// rien. `res` non nul : le niveau contextuel recoit le MEME gradient, sur sa
// propre case (coup, contexte) ; les deux niveaux estiment la meme quantite a
// des granularites differentes.
void AdaptRun(NrpaPolicy& pol, NrpaResidual* res, const NrpaRun& run,
			  float alpha, float bias_known, float hint_bias, float shrink,
			  float temp = 1.0f);

// `passes` passes d'adaptation sur chaque ligne du corpus (chantier 5bis).
void AdaptCorpus(NrpaPolicy& pol, NrpaResidual* res,
				 const std::vector<NrpaRun>& runs, uint32_t passes, float alpha,
				 float bias_known, float shrink);

// --- archive d'etats (Go-Explore, arXiv:2004.12919) ------------------------
//
// « First return, then explore » : conserver PENDANT la recherche les K
// meilleurs etats DISTINCTS, chacun avec le chemin qui y mene, puis repartir
// de chacun d'eux. Le « retour » est deja paye chez nous (rejeu du prefixe,
// restauration d'arene a 0,05 ms) ; ce qui manquait etait l'archive : le
// finisseur mono-etat fouillait le SEUL meilleur etat, mesure trois fois
// epuise en ~6 etats (l'espace y est verrouille des l'invocation).
// Cellule = hash du board complet (codes, positions, materiaux, compteurs) :
// deux lignes qui aboutissent au meme board sont confondues, seule la
// moins chere est conservee — c'est la fusion « variantes sans interet ».
struct ArchiveEntry {
	uint64_t cell = 0;       // BoardKey.hash de l'etat
	// Sous --resolve : resolutions<<44 | overlap<<36 | ~decisions — les etats
	// RIPPES d'abord (mesure : a overlap d'abord, les 8/8 muets evincent tous
	// les etats a 3 rips de l'archive, et le finisseur n'a jamais de racine
	// rippee a refermer). Sans --resolve : overlap<<40 | ~decisions.
	// En ANYTIME (optimisation de cout), les brulees s'inserent avant le
	// chemin court : resolutions<<48 | overlap<<40 | ~brulees<<32 |
	// ~decisions (et overlap<<40 | ~brulees<<32 | ~decisions sans --resolve).
	uint64_t score = 0;
	uint32_t overlap = 0;    // cartes du board cible posees
	uint32_t resolves = 0;   // resolutions exigees deja faites
	uint32_t decisions = 0;  // longueur du chemin
	uint32_t burned = 0;     // cout partiel (cimetiere + bannies), anytime
	std::vector<std::vector<uint8_t>> path;
};

// --- table de transposition partagee (lazy SMP) ----------------------------
//
// Slots atomiques a ecrasement lossy : une entree = tag 48 bits | budget
// 16 bits. Les workers LDS refaisaient le meme travail (tables privees) ; ici
// un etat resolu par l'un elague chez tous. Perdre une entree (collision de
// slot) ne coute que du travail refait ; un faux positif exigerait une
// collision de digest sur 64 bits. Taille minimale 1 Mo (2^17 slots), sans quoi
// des bits du digest ne participeraient ni au tag ni a l'index.
class SharedTT {
public:
	explicit SharedTT(size_t mb) {
		size_t want = ((mb ? mb : 1) << 20) / sizeof(std::atomic<uint64_t>);
		size_t n = 1;
		while(n * 2 <= want)
			n *= 2;
		slots = std::vector<std::atomic<uint64_t>>(n);
		for(auto& s : slots)
			s.store(0, std::memory_order_relaxed);
		mask = n - 1;
	}
	// true si l'etat a deja ete atteint avec un budget >= budget : elaguer.
	// Sinon enregistre (lossy) et rend false. Semantique identique a la table
	// privee : l'entree s'ecrit AVANT l'exploration du sous-arbre (marqueur
	// "pris"), un timeout peut donc perdre un sous-arbre reclame — c'est la
	// lossiness assumee du lazy SMP, sans effet sur les passes qui terminent.
	bool CheckAndClaim(uint64_t key, uint32_t budget) {
		std::atomic<uint64_t>& s = slots[key & mask];
		const uint64_t tag = key & ~0xffffull;
		const uint64_t cur = s.load(std::memory_order_relaxed);
		if((cur & ~0xffffull) == tag && (cur & 0xffffull) >= budget)
			return true;
		s.store(tag | (budget > 0xffffu ? 0xffffu : budget),
				std::memory_order_relaxed);
		return false;
	}

private:
	std::vector<std::atomic<uint64_t>> slots;
	uint64_t mask = 0;
};

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

	// --- elagage par nouveaute ---
	// 0 = desactive. Sinon : nombre de decisions consecutives sans atome neuf
	// au bout duquel la branche est coupee. Les resolutions de chaine passent
	// par des etats muets — couper au premier silence tuerait la ligne de
	// reference. La borne se MESURE (rapport de largeur), elle ne se devine pas.
	uint32_t novelty_patience = 0;
	// Partitionner les atomes par nombre de sous-buts atteints : la table se
	// rouvre a chaque carte cible posee (serialisation du but).
	bool novelty_serialize = true;
	// Nouveaute stricte : seul un fait jamais vu compte (cf. NoveltyTable).
	bool novelty_strict = false;
	// Couper aussi les tirages gloutons. MESURE et desactive par defaut : la
	// table est partagee entre tirages, et un tirage qui re-parcourt le meme
	// debut meurt a `patience` decisions avant d'avoir pu devier — 2/8 au lieu
	// de 6/8 sur le cas de transplantation. C'est le mode de defaillance de
	// Rollout-IW sans arbre : la variante avec arbre le contourne, mais NRPA
	// rend ici davantage pour moins de complexite.
	bool novelty_rollout_cut = false;

	// --- NRPA (tirages par politique apprise) ---
	// Niveau d'imbrication et iterations par niveau. Le cout d'un appel de
	// niveau L est iters^L tirages.
	int nrpa_level = 2;
	uint32_t nrpa_iters = 24;
	float nrpa_alpha = 1.0f;
	// Biais GNRPA d'un coup au repertoire : la place prevue pour un prior.
	float nrpa_bias_known = 1.5f;
	// Indices de domaine (--hint) : cartes canoniques dont les coups (invoquer,
	// activer, positionner) recoivent un biais supplementaire. C'est le canal
	// par lequel la connaissance du joueur ("Zalen satisfait la condition de
	// Hot Red Abyss") entre dans l'echantillonnage sans rien remodeliser.
	std::vector<uint32_t> hint_cards;
	float hint_bias = 2.0f;
	// Persistance partielle de la politique entre redemarrages : les poids sont
	// attenues par ce facteur au lieu de repartir de zero. Les redemarrages a
	// politique vierge oubliaient les sous-lignes apprises (mesure : 6/8 avec
	// les memes manquants, run apres run). 0 = comportement d'avant.
	float nrpa_restart_keep = 0.5f;
	// Meilleure sequence partagee entre workers (nul = pas de partage).
	NrpaShared* nrpa_shared = nullptr;
	// Politique INITIALE de RunNrpa (nul = vierge) : la politique fusionnee de
	// la phase tirages sert de depart aux tirages du finisseur enracines sur
	// les etats de recul — sans elle, chaque racine reapprendrait de zero.
	const NrpaPolicy* nrpa_init = nullptr;
	// Rejeu d'ADAPTATION du corpus (chantier 5bis) : sequences de decisions
	// relevees sur les lignes de solution (LiftPolicyRun), adaptees dans la
	// politique AVANT le premier tirage — `nrpa_adapt_passes` passes sur chaque
	// ligne. Le point d'injection est celui du prior par poids (politique
	// initiale du premier redemarrage) : l'A/B isole donc la FORME de
	// l'injection, prime par coup contre gradient discriminatif. 0 = inactif.
	const std::vector<NrpaRun>* nrpa_adapt_runs = nullptr;
	uint32_t nrpa_adapt_passes = 0;
	// Politique a DEUX NIVEAUX (chantier 5ter) : retenue du niveau contextuel,
	// s = n/(n+k). k negatif = mecanisme ETEINT (comportement d'avant, la case
	// contextuelle n'est ni lue ni ecrite). k = 0 : le contexte prend la main
	// des la premiere mise a jour ; k grand : il faut beaucoup d'evidence.
	float ctx_shrink = -1.0f;
	// TEMPERATURE de l'echantillonnage (GNRPA, arXiv:2003.10024) : les logits
	// sont divises par tau avant le softmax. C'est le seul levier connu qui
	// agisse sur la MASSE et non sur le CLASSEMENT — or la mesure de la session
	// 7bis dit que le classement etait deja bon (coup du corpus 1er dans 96 %
	// des cas a politique vierge) et que c'est la masse qui manque (44 % de
	// probabilite moyenne, soit 0,44^160 sur une ligne entiere). tau < 1
	// concentre, tau = 1 = comportement d'avant. L'adaptation utilise la MEME
	// temperature, sans quoi le gradient ne serait pas celui de la
	// distribution echantillonnee.
	float nrpa_temp = 1.0f;
	// GNRPA a repetitions limitees (arXiv:2401.10420) : nombre de fois ou la
	// meilleure sequence peut etre RE-TROUVEE (meme score MATERIEL — la part
	// nouveaute du score decroit a chaque rejeu, l'egalite stricte ne se
	// produirait jamais) avant d'arreter le niveau. Re-trouver la meme ligne
	// signale la convergence AVANT que la stagnation (8 iterations sans
	// progres) ne l'admette. 0 = ancien comportement, stagnation seule.
	uint32_t nrpa_lr = 0;
	// PHS* (arXiv:2103.11505, meme papier que LTS) : poids de la distance au
	// but dans le cout du finisseur — cout = log(d+1) + levin_h * h(n) - log
	// pi(n), h = cartes cibles manquantes + resolutions manquantes au noeud
	// developpe (heritee par ses enfants). Le Levin pur (0) est aveugle au
	// but : il re-monte les reculs profonds sans preferer les branches qui
	// ripent ou qui posent. 0 = Levin pur.
	float levin_h = 1.0f;
	// sqrt-LTS (arXiv:2412.05196) : re-enraciner la recherche du finisseur a
	// chaque INDICE. Le cout de Levin d(n)/pi(n) est remplace par le cout
	// enracine lambda/pi(n ; n_k) ou n_k est l'ancetre-indice le plus proche —
	// la probabilite repart de 1 a chaque indice, au lieu de se multiplier sur
	// toute la ligne. Notre indice est deja calcule a chaque noeud : le nombre
	// de cartes du board cible posees CHANGE.
	//
	// Le min sur les ancetres de l'article se reduit ici a un seul terme, et
	// c'est EXACT et non une approximation : a poids uniformes sur les indices,
	// pour n_j precedant n_k, lambda/pi(n;n_j) >= (1/pi(n_k|n_j)) *
	// lambda/pi(n;n_k) >= lambda/pi(n;n_k) — l'ancetre-indice le PLUS PROCHE
	// minimise toujours. Un seul lambda par noeud suffit donc.
	//
	// Prevision mesuree avant implementation (ForecastSearchCost, corpus
	// sF_final) : borne monolithique 10^26 a 10^60 expansions selon la
	// politique, borne decomposee sur les 18 segments 10^5,8 a 10^12,5.
	// false = cout de Levin d'avant, bit pour bit.
	bool levin_reroot = false;
	// Poids d'une resolution exigee (--resolve) dans le gradient des tirages.
	// A 100 (une carte cible), les lignes 8/8 SANS rip gagnent la course
	// d'adaptation contre les lignes rip-partielles (mesure session 4 :
	// 850 k tirages enracines, zero rip converti) ; a 250, un rip vaut 2,5
	// cartes posees et une ligne 5 cartes + 2 rips bat une ligne 8/8 muette.
	float resolve_weight = 250.0f;
	// Archive Go-Explore : nombre d'etats DISTINCTS (cellule = board complet)
	// conserves avec leur chemin pendant la recherche. 0 = pas d'archive.
	size_t archive_k = 0;

	// --- reparation : resynchronisation semantique ---
	// digest -> index dans yrp.responses (cf. LiftRefLine). Nul = pas de
	// resynchronisation : apres une deviation, le suffixe enregistre reste
	// aveugle (l'etat mesure : echanger les invocations #4/#5 est introuvable
	// jusqu'a k=12).
	const std::unordered_map<uint64_t, size_t>* ref_digests = nullptr;
	// plan_key par index de reponse (cf. LiftRefLine) : le repertoire FENETRE.
	// Apres une premiere deviation, un coup que la reference joue a moins de
	// `repair_window` decisions du point courant est gratuit. Jamais sur le
	// prefixe pur : l'invariant "0 ecart retrouve la reference" est preserve.
	const std::vector<uint64_t>* ref_keys = nullptr;
	uint32_t repair_window = 24;

	// --- table de transposition partagee entre workers (lazy SMP) ---
	// Nul = table privee par worker (comportement d'avant).
	SharedTT* shared_tt = nullptr;

	// --- contraintes de ligne ---
	// n-ieme invocation (1-base, normales + speciales — le decompte de Nibiru,
	// les flips n'y comptent pas) -> codes CANONIQUES admis. Une invocation
	// d'index contraint dont la carte n'est pas dans la liste ELAGUE la
	// branche : la contrainte reduit l'espace au lieu de le filtrer apres coup.
	// Semantique conditionnelle : une ligne qui n'atteint pas la n-ieme
	// invocation n'est pas en faute.
	std::map<uint32_t, std::vector<uint32_t>> summon_constraints;
	// Garde : a partir de la `guard_after`-ieme invocation, a chaque fenetre
	// de reponse de l'ADVERSAIRE — la et seulement la ou Nibiru peut tomber —
	// au moins une clause doit tenir. Les creux transitoires en pleine
	// resolution (le garde part en materiel pendant que son remplacant arrive)
	// ne sont PAS des fautes : l'adversaire ne peut pas y agir. C'est ce qui
	// rend la garde jouable ; l'exiger a chaque decision interdirait de
	// convertir un garde en un autre.
	uint32_t guard_after = 0;
	std::vector<GuardClause> guard_clauses;
	// Extinction de la garde : au-dela d'un certain point, la menace n'existe
	// plus — un deck qui vide la main adverse (handrip) n'a plus Nibiru a
	// craindre. La garde n'est pas exigee aux fenetres ou la main adverse
	// compte au plus ce nombre de cartes. -1 = jamais eteinte.
	int guard_opp_hand_release = -1;

	// Minimum de resolutions d'effet par carte. Compte les ACTIVATIONS
	// (MSG_CHAINING) : en solitaire rien ne nie une chaine, l'activation vaut
	// resolution — filtrees par zone d'activation (cf. ResolveReq). Contrainte
	// de MINIMUM : elle ne peut pas elaguer en cours de ligne (l'avenir peut
	// encore l'accomplir), elle se controle AU BUT — un board conforme sans les
	// resolutions n'est pas une solution, et la recherche continue. Au plus 4
	// cartes (compteurs empaquetes 16 bits x 4 dans un uint64 de chemin).
	std::vector<ResolveReq> resolve_min;
	// Contrainte de MATERIAU : quand cette carte (canonique) est invoquee, ses
	// materiaux — les MSG_MOVE marques REASON_SYNCHRO|REASON_MATERIAL dans la
	// meme resolution — doivent inclure au moins une carte dont l'attribut
	// intersecte le masque ("Chaos Angel invoque avec un monstre LUMIERE").
	// Violation = branche coupee : une invocation ratee ne se defait pas.
	std::vector<std::pair<uint32_t, uint32_t>> material_req;

	// Compteurs initiaux, pour une recherche qui demarre au MILIEU d'une ligne
	// (finisseur : rejouer un prefixe puis fouiller depuis son etat). Sans eux,
	// les contraintes d'invocation et la coupure de tour compteraient depuis
	// zero alors que le prefixe a deja invoque et entame le tour 1.
	uint32_t initial_summons = 0;
	uint32_t initial_turns = 0;
	uint64_t initial_resolved = 0;   // compteurs de resolutions empaquetes

	// --- objectif de COUT anytime (optimisation lexicographique) ---
	// La recherche ne s'arrete plus a la premiere solution : chaque solution
	// resserre la borne, l'ensemble conserve est borne par remplacement du
	// PIRE (cout lexicographique : brulees, puis actions, puis decisions,
	// dedup par chemin), et le score de but NRPA devient lexicographique —
	// une ligne moins chere a un meilleur score, l'adaptation tire vers elle
	// (recette Montparnasse, arXiv:2505.02110). Les tirages CONTINUENT apres
	// le but : des decisions de plus peuvent REDUIRE les brulees (les
	// recuperations reelles — piege 27 — jouent dans les deux sens), et
	// chaque re-atteinte du board re-enregistre si elle est moins chere.
	bool anytime = false;
	// Borne brulees (B&B) : un etat dont les brulees COURANTES depassent
	// meilleures_brulees_connues + burn_slack est coupe (tirages seulement).
	// Les brulees ne sont PAS monotones le long d'une ligne (recuperations) :
	// la marge absorbe les recuperations, elle se MESURE sur la reference
	// (rapport « brulees max en cours de ligne »). >= 255 = borne inactive.
	uint32_t burn_slack = 6;
	// Graine de la borne : meilleures brulees deja connues AVANT la recherche
	// (solutions des phases precedentes). 0 = aucune.
	uint32_t burn_limit = 0;
	// Borne brulees PARTAGEE entre workers : meilleures brulees GLOBALES
	// (UINT32_MAX = aucune). Sans elle, un worker qui trouve 19 ne coupe rien
	// chez les quinze autres : chaque amelioration se publie (CAS min), et la
	// coupure lit la borne globale + burn_slack a chaque test (charge relaxed,
	// negligeable devant les deux Count() du test). Nul = borne locale seule.
	std::atomic<uint32_t>* shared_burn = nullptr;

	// --- buts ALTERNATIFS (test adverse --fire) ---
	// Boards egalement acceptes au but : le board cible MOINS chaque
	// sous-ensemble des cartes sacrifiees pour contrer la menace (arbitrage
	// du joueur : contrer Nibiru par Zalen consomme Junk Signal — et peut
	// couter une piece de construction en plus). Nul = comportement d'avant.
	// Le pointeur doit survivre a la recherche.
	const std::vector<BoardKey>* target_alts = nullptr;
};

struct Solution {
	std::vector<std::vector<uint8_t>> responses;
	uint32_t actions = 0;      // invocations + activations (tier 2)
	uint32_t decisions = 0;    // tier 3
	uint32_t burned = 0;       // cartes au cimetiere + bannies (tier 1)
	uint32_t hand_left = 0, deck_left = 0, extra_left = 0;
	// Atteinte par le but ALTERNATIF (cfg.target_alt — board sans la carte
	// sacrifiee) et non par le board complet.
	bool alt = false;
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
	// Les reponses qui MENENT a ce meilleur etat. C'est la matiere premiere du
	// finisseur : rejouer ce chemin et fouiller exhaustivement depuis son etat,
	// la ou l'echantillonnage sait monter mais rate le dernier pas.
	std::vector<std::vector<uint8_t>> best_path;
	// Le terrain du meilleur etat, EN DETAIL (positions, materiaux, compteurs).
	// Quand les 8 codes y sont mais que le but ne se declenche pas, c'est ici
	// que se lit CE QUI differe — un compte ne se corrige pas, un detail si.
	std::vector<QueriedCard> best_mzone, best_szone;
	// --- elagage par nouveaute ---
	uint64_t novelty_novel = 0;     // etats ayant produit un atome inedit
	uint64_t novelty_stale = 0;     // etats muets (aucun atome neuf)
	uint64_t novelty_cuts = 0;      // branches coupees, patience epuisee
	size_t novelty_atoms = 0;       // taille finale de la table d'atomes
	// --- contraintes ---
	uint64_t constraint_cuts = 0;   // branches coupees par --summon
	uint64_t guard_cuts = 0;        // branches coupees par la garde (--guard)
	// --- objectif de cout anytime ---
	uint64_t burn_cuts = 0;         // tirages coupes par la borne brulees
	// sqrt-LTS : nombre de noeuds developpes qui ont RE-ENRACINE la recherche
	// (un indice y est tombe). Sans ce compteur, un rerooting inactif serait
	// indiscernable d'un rerooting inutile — piege 40.
	uint64_t reroots = 0;
	uint64_t goal_hits = 0;         // atteintes du but (re-atteintes comprises)
	// --- reparation ---
	// Resynchronisations semantiques : etats dont le digest a retrouve un point
	// PLUS LOIN de la reference, rendant le suffixe enregistre a nouveau
	// lisible apres deviation.
	uint64_t resyncs = 0;
	// --- tirages ---
	uint64_t rollout_count = 0;
	uint64_t turn_cuts = 0;         // tirages arretes au changement de tour
	uint64_t nrpa_adapts = 0;
	// Visibilite des indices (--hint) : dans combien d'etats un coup indice
	// etait LEGAL, et combien de fois il a ete pris. hint_seen = 0 signifie
	// que le probleme n'est pas l'echantillonnage mais la LEGALITE — le core
	// ne propose jamais l'invocation, les materiaux n'y sont pas.
	uint64_t hint_seen = 0, hint_taken = 0;
	// Tirages ayant atteint >= k resolutions exigees (k = 1..4). LE
	// diagnostic du handrip : separe « la politique ne rippe jamais »
	// (echantillonnage, resolve_reached[0] > 0) de « le rip n'est jamais
	// legal/possible ici » (jeu, resolve_reached[0] = 0).
	uint64_t resolve_reached[4] = { 0, 0, 0, 0 };
	// Meilleure crete CONDITIONNEE aux resolutions COMPLETES : jusqu'ou les
	// lignes qui ont fait TOUS les rips montent-elles ? Si elles plafonnent
	// loin du board pendant que les lignes muettes font 8/8, les deux buts
	// sont probablement incompatibles en RESSOURCES — question de jeu (ou de
	// relaxation arithmetique a prouver), plus de recherche.
	uint32_t best_overlap_ripped = 0;
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

	// NRPA (Cazenave) : tirages par POLITIQUE APPRISE au lieu d'evaluations de
	// fils. Un poids par code de coup — nos plan_key sont exactement cela — et
	// une adaptation vers la meilleure sequence a chaque niveau. Le repertoire
	// entre comme biais (GNRPA), pas comme prime : il orientait a tort la
	// descente vers "fin de tour" quand il etait additif.
	//
	// Deux gains distincts : la politique concentre les tirages, et l'absence
	// d'evaluation des fils (avancer/mesurer/restaurer sur CHAQUE fils) rend
	// chaque decision plusieurs fois moins chere.
	void RunNrpa(const BoardKey& target, const std::vector<PlanStep>& plan,
				 uint64_t seed);

	// FINISSEUR : Levin Tree Search (arXiv:2103.11505) depuis la position
	// courante — recherche best-first COMPLETE ordonnee par le cout
	// d(n)/pi(n), ou pi(n) est le produit des probabilites softmax de la
	// politique NRPA le long du chemin. Le nombre d'expansions avant de
	// trouver une solution est borne par la qualite de la politique : c'est
	// le « chercheur d'aiguilles » principled que les tirages ne seront
	// jamais — l'echantillonnage rate une sequence rare, l'enumeration
	// ordonnee par la politique la trouve dans l'ordre de sa probabilite.
	//
	// Les coups FORCES (fenetres adverses, selections a candidat unique) sont
	// joues en ligne et ne coutent ni profondeur ni probabilite (une decision
	// forcee coute zero — meme principe que la reparation). Un noeud se rejoue
	// depuis la racine par son chemin de decisions : restauration 0,05 ms puis
	// rejeu du suffixe — c'est le « retour » de Go-Explore, deja paye.
	// Utilise cfg.initial_* (la recherche demarre au milieu d'une ligne).
	void RunLevin(const BoardKey& target, const std::vector<PlanStep>& plan,
				  const NrpaPolicy& policy);

	const SearchStats& Stats() const { return stats; }
	const std::vector<Solution>& Solutions() const { return solutions; }
	// Archive Go-Explore collectee pendant la recherche (cfg.archive_k > 0).
	const std::vector<ArchiveEntry>& Archive() const { return archive; }
	// Politique apprise par RunNrpa, exportee en fin de run — elle guidait
	// les tirages, elle guide ensuite le finisseur (RunLevin).
	const NrpaPolicy& LearnedPolicy() const { return final_policy; }

private:
	enum class Step { Prompt, Ended, Rejected };

	// Politique NRPA : un poids par plan_key, echantillonnage softmax.
	// (PolicyStep / NrpaRun vivent au niveau de l'espace de noms : la meilleure
	// sequence se partage entre workers via NrpaShared.)
	using Policy = NrpaPolicy;

	Step StepToPrompt();
	uint64_t Digest() const;
	void Descend(uint32_t depth, uint32_t actions);
	bool DescendGuided(uint32_t depth, uint32_t actions, uint32_t turns,
					   uint32_t summons, uint64_t resolved);
	bool DescendRepair(uint32_t depth, uint32_t actions, size_t ref_index,
					   uint32_t disc, uint32_t stale, uint32_t turns,
					   uint32_t summons, uint64_t resolved);
	bool DescendTransplant(uint32_t depth, uint32_t actions, uint32_t disc,
						   uint32_t stale, uint32_t turns, uint32_t summons,
						   uint64_t resolved);
	// Verifie les invocations du dernier StepToPrompt contre les contraintes,
	// `before` etant le nombre d'invocations deja faites sur ce chemin. Rend
	// false si une contrainte est violee (la branche doit mourir).
	bool SummonsOk(uint32_t before) const;
	// Fenetre d'intervention de l'adversaire sous garde active : rend true si
	// la garde ne tient pas et que la branche doit etre coupee.
	bool GuardCut(const BoardKey& here, uint32_t summons);
	// Un tirage, de la position courante jusqu'a la fin du tour. Renvoie true
	// si le board cible a ete atteint.
	bool Rollout(uint64_t& rng);
	// Un tirage sous politique : pas d'evaluation des fils, le choix est tire
	// au sort selon exp(poids + biais). Remplit `run` pour l'adaptation.
	void PolicyRollout(uint64_t& rng, const Policy& pol, NrpaRun& run);
	// La politique n'est copiee qu'une fois par appel de niveau (elle etait
	// copiee A CHAQUE TIRAGE : une table de milliers d'entrees par rollout).
	double Nrpa(int level, const Policy& pol, NrpaRun& best, uint64_t& rng);
	// Niveau superieur : adapte la politique PERSISTANTE (par reference, pour
	// la persistance entre redemarrages) et echange la meilleure sequence avec
	// les autres workers (cfg.nrpa_shared).
	double NrpaTop(Policy& pol, NrpaRun& best, uint64_t& rng);
	void Adapt(Policy& pol, const NrpaRun& best);
	// Enumere le prompt courant dans `out` (adversaire : "ne rien faire" seul ;
	// prompt non enumerable : reponse par defaut). false = branche morte.
	bool FillChoices(ChoiceList& out);
	// Une ChoiceList par profondeur, reutilisee entre freres et entre passes :
	// le deque garantit la stabilite des references pendant la recursion.
	ChoiceList& ChoicesAt(uint32_t depth) {
		while(choice_pool.size() <= depth)
			choice_pool.emplace_back();
		return choice_pool[depth];
	}
	// Nombre d'entrees du board cible deja en place : distance au but.
	// Prend le board deja calcule — c'etait le point chaud : deux requetes de
	// zone par fils evalue, dont une redondante avec ComputeBoardKey.
	uint32_t Heuristic(const BoardKey& here) const;
	// Suivi de la meilleure approche + test de but. Rend true si `here` EST la
	// cible ET que les minimums de resolutions sont atteints. Factorise ce que
	// chaque strategie dupliquait. En mode anytime, l'enregistrement passe par
	// remplacement du pire et dedup par chemin ; le cout du but atteint reste
	// lisible dans goal_burned/goal_actions/goal_depth (le score NRPA le lit).
	bool GoalCheck(const BoardKey& here, uint32_t depth, uint32_t actions,
				   uint64_t resolved);
	// Brulees courantes du joueur cible (cimetiere + bannies) : le cout de
	// tier 1, lu au but et par la borne B&B.
	uint32_t CurrentBurned();
	// Borne brulees effective : la locale, resserree par la borne PARTAGEE
	// entre workers si elle existe (cfg.shared_burn, anytime seulement).
	uint32_t EffectiveBurnCut() const;
	// Progres vers les minimums de resolutions, plafonne : sert de gradient
	// aux tirages (sans lui, NRPA n'a aucune raison de resoudre Omega deux
	// fois avant de fermer le board).
	uint32_t ResolveProgress(uint64_t resolved) const;
	// Elagage par nouveaute : observe l'etat, met a jour le compteur de
	// silence. Rend true si la branche doit etre coupee. `resolved` entre dans
	// la partition : la table se ROUVRE apres chaque resolution exigee
	// (--resolve) — serialisation du but sur les deux dimensions, cartes
	// posees ET rips faits.
	bool NoveltyCut(const BoardKey& here, uint32_t depth, uint64_t resolved,
					uint32_t& stale);
	// Archive Go-Explore : propose l'etat courant (chemin = `path`). A appeler
	// APRES les controles de garde et de tour — un etat qui viole la garde ou
	// deborde du tour 1 est un point de depart condamne d'avance.
	void ArchiveObserve(const BoardKey& here, uint32_t depth, uint64_t resolved);
	bool BudgetExhausted() const;

	Duel& duel;
	Arena& arena;
	const Replay& yrp;
	SearchConfig cfg;
	// Somme des minimums de resolutions exigees (cf. best_overlap_ripped).
	uint32_t resolve_total = 0;

	// Etat du prompt courant
	uint8_t prompt_type = 0;
	std::vector<uint8_t> prompt_payload;
	int prompt_player = -1;
	uint32_t actions_this_step = 0;
	// Cartes invoquees (normal + special) par le dernier StepToPrompt, dans
	// l'ordre. Codes bruts du message ; 0 = invoquee face verso (inconnue).
	std::vector<uint32_t> summons_this_step;
	// Resolutions (activations) de cartes SURVEILLEES par le dernier
	// StepToPrompt, empaquetees : 16 bits par entree de cfg.resolve_min.
	uint64_t resolved_this_step = 0;
	// Une invocation surveillee (--material) a viole sa contrainte de materiau
	// pendant le dernier StepToPrompt : la branche doit mourir.
	bool material_violation = false;
	// Materiaux (codes) envoyes par la resolution en cours, pour attribution a
	// l'invocation qui suit.
	std::vector<uint32_t> recent_materials;
	// Changements de tour vus par le dernier StepToPrompt. Le board cible est
	// celui de la fin du tour 1 : au-dela, il est fige et tout etat explore est
	// du temps perdu.
	uint32_t turns_this_step = 0;
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
	// Table de nouveaute, partagee par toutes les branches d'une meme passe :
	// c'est sa globalite qui fusionne les lignes "distinctes mais sans interet
	// distinct" que la transposition laisse passer.
	NoveltyTable novelty;
	std::vector<uint64_t> atoms_scratch;
	std::vector<Solution> solutions;
	std::vector<std::vector<uint8_t>> path;
	SearchStats stats;
	std::chrono::steady_clock::time_point start;

	// --- objectif de cout anytime ---
	// Cout du dernier but atteint (GoalCheck), pour le score NRPA.
	uint32_t goal_burned = 0, goal_actions = 0, goal_depth = 0;
	// Meilleur cout lexicographique vu par CETTE recherche, et borne brulees
	// effective (meilleures brulees + burn_slack ; UINT32_MAX = inactive).
	uint64_t best_cost_key = UINT64_MAX;
	uint32_t best_burned_seen = UINT32_MAX;
	uint32_t burn_cut = UINT32_MAX;
	// Dedup des solutions par chemin : une politique convergee rejoue la meme
	// ligne des milliers de fois, l'ensemble n'en garde qu'une.
	std::unordered_set<uint64_t> solution_hashes;

	// Archive Go-Explore (cfg.archive_k > 0) : une entree par cellule (board
	// complet), remplacement de la pire quand l'archive est pleine. Le seuil
	// `archive_min_score` rend le cas courant (etat sans interet) gratuit :
	// une comparaison d'entiers, pas de hachage.
	std::vector<ArchiveEntry> archive;
	std::unordered_map<uint64_t, size_t> archive_cells;
	uint64_t archive_min_score = 0;
	// Politique finale du run NRPA (exportee pour le finisseur).
	Policy final_policy;
	// Niveau CONTEXTUEL de la politique (chantier 5ter, cfg.ctx_shrink >= 0).
	// Vide et jamais consulte quand le mecanisme est eteint.
	NrpaResidual ctx_weights;

	// Tampons reutilises des chemins chauds (une allocation par decision est
	// une allocation de trop a des dizaines de millions de decisions par run).
	std::deque<ChoiceList> choice_pool;   // recherches recursives, par profondeur
	ChoiceList ro_choices;                // tirages (aucune recursion)
	std::vector<Message> msgs_scratch;    // StepToPrompt
	BoardKey board_scratch;               // board du noeud courant
	BoardKey child_board_scratch;         // board d'un fils evalue
};

} // namespace solver
