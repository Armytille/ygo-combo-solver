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
	uint32_t nrpa_lr = 0;
	// Table de transposition PARTAGEE entre workers (lazy SMP), en Mo par
	// passe. 0 = tables privees (comportement d'avant).
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
	bool AnyBoardEdit() const {
		return !board_add.empty() || !board_remove.empty();
	}
	bool Any() const {
		return !summons.empty() || !guard.empty() || !no_activate.empty() ||
			   !no_chain.empty() || !resolve_min.empty() ||
			   !material_req.empty();
	}
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
		"  --material <spec>  l'invocation de cette carte doit consommer au\n"
		"                     moins un materiau de ces attributs. <spec> =\n"
		"                     carte:attr[,attr...], attributs : lumiere tenebres\n"
		"                     terre eau feu vent divin. Repetable.\n"
		"                     Ex : --material \"Chaos Angel:lumiere\"\n"
		"  --outdir <dir>     ou ecrire les replays produits (defaut solutions/)\n"
		"  --solve            recherche guidee vers le board cible\n"
		"  --solve-ms <ms>    budget temps de la recherche (defaut 120000)\n"
		"  --threads <n>      workers de recherche (defaut : tous les coeurs)\n"
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
		"  --nrpa-lr <n>      repetitions limitees (GNRPA-LR) : arrete un niveau\n"
		"                     apres n re-trouvailles de la meilleure sequence\n"
		"                     (defaut 0 = stagnation seule — R=2 mesure perdant :\n"
		"                     8/8 -> 7/8 sur la transplantation test 4)\n"
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
		} else if(a == "--nrpa-lr") {
			const char* v = next("--nrpa-lr"); if(!v) return false;
			o.nrpa_lr = static_cast<uint32_t>(std::atoi(v));
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

	for(const std::string& spec : opt.board_add_specs) {
		size_t at = spec.rfind('@');
		std::string card = (at == std::string::npos) ? spec : spec.substr(0, at);
		std::string pos = (at == std::string::npos)
			? "ATK" : Trimmed(spec.substr(at + 1));
		uint32_t position = 0;
		if(pos == "ATK")      position = POS_FACEUP_ATTACK;
		else if(pos == "DEF") position = POS_FACEUP_DEFENSE;
		else {
			std::printf("!! --board-add : position inconnue \"%s\" (ATK ou DEF)\n",
						pos.c_str());
			return false;
		}
		uint32_t code = 0;
		if(!ResolveCard(Trimmed(card), db, "--board-add", code))
			return false;
		out.board_add.emplace_back(code, position);
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
	// Suivi du prompt courant, seulement si des contraintes sont a juger.
	const bool track = cons && cons->Any();
	uint8_t ptype = 0;
	int pplayer = -1;
	std::vector<uint8_t> ppayload;
	EnumOptions peo;
	if(track) {
		peo.no_activate = cons->no_activate.empty() ? nullptr : &cons->no_activate;
		peo.db = &duel.Db();
	}

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
					r.summons_at_target = r.summon_codes.size();
					r.burned_at_target =
						duel.Count(uint8_t(opt.target_player), LOCATION_GRAVE) +
						duel.Count(uint8_t(opt.target_player), LOCATION_REMOVED);
					r.have_target = true;
				}
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

			if(track && IsPrompt(m.type)) {
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
	if(r.have_target)
		std::printf("  brulees             : %u au board, pic %u en cours de "
					"ligne (marge de recuperation %d — calibre --burn-slack)\n",
					r.burned_at_target, r.burned_max,
					static_cast<int>(r.burned_max) -
						static_cast<int>(r.burned_at_target));

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
	cfg.target_player = opt.target_player;
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
	cfg.enumeration.max_subsets = 24;
	cfg.summon_constraints = cons.summons;
	cfg.guard_after = cons.guard_after;
	cfg.guard_clauses = cons.guard;
	cfg.guard_opp_hand_release = cons.guard_opp_hand_release;
	cfg.resolve_min = cons.resolve_min;
	cfg.material_req = cons.material_req;
	cfg.hint_cards = cons.hints;
	if(!cons.no_activate.empty()) {
		cfg.enumeration.no_activate = &cons.no_activate;
		// Le filtre compare des codes canoniques : il faut la table des alias.
		cfg.enumeration.db = &db;
	}
	if(!cons.no_chain.empty())
		cfg.enumeration.no_chain = &cons.no_chain;
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
		LiftRefLine(duel, arena, yrp, opt.target_player, ref_decisions,
					cfg.enumeration, ref_digests, ref_keys);
		arena.Restore();
		size_t known = 0;
		for(uint64_t k : ref_keys)
			known += k != 0;
		std::printf("  ligne relevee  : %zu digests, %zu/%zu coups identifies "
					"(%.0f ms)\n", ref_digests.size(), known, ref_keys.size(),
					MsSince(t0));
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

	struct PassOut {
		std::vector<Solution> found;
		uint64_t nodes = 0, transpos = 0, cuts = 0, resyncs = 0;
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
		// Un jeton par point de deviation possible le long de l'echine.
		std::vector<std::atomic<uint32_t>> claims(ref_decisions + 1);
		for(auto& c : claims)
			c.store(0, std::memory_order_relaxed);
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
					wcfg.novelty_patience = pat;
					wcfg.shared_tt = stt.get();
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
						out.found.push_back(x);
					out.nodes += s.Stats().nodes;
					out.transpos += s.Stats().transpositions;
					out.cuts += s.Stats().novelty_cuts;
					out.resyncs += s.Stats().resyncs;
					out.timed_out |= s.Stats().hit_time_limit;
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
		out.ms = MsSince(t0);
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
		std::printf("  %-8u %10zu %12llu %11llu %10llu %8.1f s%s%s\n", k,
					o.found.size(), (unsigned long long)o.nodes,
					(unsigned long long)o.transpos, (unsigned long long)o.cuts,
					o.ms / 1000.0, o.timed_out ? "  (budget epuise)" : "", resync);
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
	size_t written = 0, rejected = 0;
	size_t rej_retry = 0, rej_cons = 0, rej_board = 0;
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
			return;
		{
			Duel d(db, scripts, &a);
			if(!d.Create(start_yrp.seed, start_yrp.duel_flags, start_yrp.start_lp,
						 start_yrp.start_hand, start_yrp.draw_count, err) ||
			   !d.Setup(start_yrp, err, opp_hand,
						static_cast<uint8_t>(1 - opt.target_player)))
				return;
			if(opt.stop_gc)
				d.SetLuaGc(false);
			a.Push();
			for(size_t i = 0; i < sols.size() && i < 16; ++i) {
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
				for(const auto& [n, allowed] : cons.summons) {
					if(n > summons.size())
						continue;
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
					else if(!cons_ok)
						++rej_cons;
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
	std::printf("  %zu replay(s) ecrits dans %s\n", written, outdir.c_str());
	if(rejected)
		std::printf("  %zu rejetee(s) : %zu MSG_RETRY, %zu contrainte(s), "
					"%zu board non conforme\n", rejected, rej_retry, rej_cons,
					rej_board);
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
		eo.max_subsets = 24;
		eo.db = &db;
		LiftPlan(duel, arena, yrp, opt.target_player, ref.target_at, eo, plan);
		arena.Restore();
	}

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
			return;
		{
			Duel fd(db, scripts, &fa);
			if(fd.Create(fyrp->seed, fyrp->duel_flags, fyrp->start_lp,
						 fyrp->start_hand, fyrp->draw_count, err) &&
			   fd.Setup(*fyrp, err, extra, oppo)) {
				if(opt.stop_gc)
					fd.SetLuaGc(false);
				EnumOptions oeo;   // enumeration ADVERSE : brute, sans nos filtres
				oeo.dedup_by_code = true;
				oeo.max_subsets = 24;
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
				return;
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
								reo.max_subsets = 24;
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
						fcfg.max_decisions =
							ref.target_at * 3 / 2 + 32 > W.prefix.size()
								? static_cast<uint32_t>(
									  ref.target_at * 3 / 2 + 32 -
									  W.prefix.size())
								: 64u;
						fcfg.max_actions = 0;
						fcfg.time_limit_ms = opt.fire_ms;
						fcfg.max_nodes = 50000000;
						fcfg.max_solutions = 4;
						fcfg.enumeration.dedup_by_code = true;
						fcfg.enumeration.max_subsets = 24;
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
									deo.max_subsets = 24;
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
				}
			}
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
void RunTransplantSolve(Duel& duel, const Replay& ref_yrp, const Replay& start_yrp,
						const Options& opt, Arena& arena, const LineResult& ref,
						CardDB& db, ScriptProvider& scripts, uint32_t patience,
						const LineConstraints& cons) {
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
	if(cons.AnyBoardEdit()) {
		auto mz = ref.target_self.mzone;
		auto sz = ref.target_self.szone;
		bool edit_ok = true;
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
		target = MakeBoardKey(mz, sz, db);
		std::printf("\n--- board cible EDITE ---\n");
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
	cfg.summon_constraints = cons.summons;
	cfg.guard_after = cons.guard_after;
	cfg.guard_clauses = cons.guard;
	cfg.guard_opp_hand_release = cons.guard_opp_hand_release;
	cfg.resolve_min = cons.resolve_min;
	cfg.material_req = cons.material_req;
	cfg.hint_cards = cons.hints;
	cfg.levin_h = static_cast<float>(opt.levin_h);
	cfg.resolve_weight = static_cast<float>(opt.resolve_weight);
	// Optimisation de cout anytime : la recherche continue apres la premiere
	// solution (chaque solution resserre la borne), l'ensemble par worker est
	// borne par remplacement du pire, le score de but NRPA est lexicographique.
	if(opt.optimize) {
		cfg.anytime = true;
		cfg.max_solutions = 24;
		cfg.burn_slack = opt.burn_slack;
		cfg.burn_limit = opt.burn_limit;
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
	auto merge_archive = [&](const std::vector<ArchiveEntry>& a) {
		for(const ArchiveEntry& e : a) {
			auto [it, fresh] = global_archive.try_emplace(e.cell, e);
			if(!fresh && e.score > it->second.score)
				it->second = e;
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
		double budget = (std::max)(0.0, (opt.solve_ms - spent) * 0.7);
		// Budget reserve au finisseur (--finisher-min) : les tirages cedent
		// la place quand la conversion est la question.
		if(opt.finisher_min > 0)
			budget = (std::max)(0.0, (std::min)(
				budget, opt.solve_ms - spent - opt.finisher_min));
		std::mutex merge;
		struct ModeStats {
			uint64_t nodes = 0, rollouts = 0, cuts = 0, turn_cuts = 0, adapts = 0;
			uint64_t hint_seen = 0, hint_taken = 0;
			uint64_t rr[4] = { 0, 0, 0, 0 };
			uint32_t overlap = 0, monsters = 0, overlap_ripped = 0;
		};
		ModeStats greedy, nrpa;
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
		// Meilleure sequence GLOBALE, partagee entre les workers NRPA : les
		// redemarrages repartent de la meilleure ligne connue de tous au lieu
		// de reapprendre les memes sous-lignes chacun dans son coin.
		NrpaShared shared_best;
		auto t0 = Clock::now();

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
				return;
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
					// Un budget long merite un niveau d'imbrication de plus :
					// l'exploitation de NRPA croit avec la profondeur de
					// recursion, et le garde-fou de stagnation borne le risque.
					wcfg.nrpa_level = (budget > 180000.0) ? 3 : 2;
					if(opt.nrpa_bias >= 0)
						wcfg.nrpa_bias_known = static_cast<float>(opt.nrpa_bias);
					wcfg.nrpa_restart_keep = static_cast<float>(opt.nrpa_keep);
					wcfg.nrpa_shared = &shared_best;
					wcfg.nrpa_lr = opt.nrpa_lr;
					wcfg.archive_k = opt.archive_k;
					Search s(local, la, start_yrp, wcfg);
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
					m.adapts += s.Stats().nrpa_adapts;
					m.hint_seen += s.Stats().hint_seen;
					m.hint_taken += s.Stats().hint_taken;
					for(int k = 0; k < 4; ++k)
						m.rr[k] += s.Stats().resolve_reached[k];
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
				}
			}
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
		std::printf("  total : %.1f s, au mieux %u des %zu cartes cibles, "
					"%u monstre(s)\n", secs, best_overlap, target.codes.size(),
					best_monsters);
		if(!cfg.hint_cards.empty())
			std::printf("  visibilite des indices : legaux dans %llu etat(s), "
						"pris %llu fois%s\n",
						(unsigned long long)(nrpa.hint_seen + greedy.hint_seen),
						(unsigned long long)(nrpa.hint_taken + greedy.hint_taken),
						(nrpa.hint_seen + greedy.hint_seen) == 0
							? "  <-- JAMAIS LEGAL : le probleme est la "
							  "disponibilite des materiaux, pas la recherche"
							: "");
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
				return;
			{
				Duel fd(db, scripts, &fa);
				if(!fd.Create(start_yrp.seed, start_yrp.duel_flags,
							  start_yrp.start_lp, start_yrp.start_hand,
							  start_yrp.draw_count, err) ||
				   !fd.Setup(start_yrp, err,
							 cons.opp_hand.empty() ? nullptr : &cons.opp_hand,
							 static_cast<uint8_t>(1 - opt.target_player)))
					return;
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
						cfg.max_decisions > best_path.size()
							? static_cast<uint32_t>(cfg.max_decisions -
													best_path.size())
							: 64u;
					fcfg.initial_summons = pc.summons;
					fcfg.initial_turns = pc.turns;
					fcfg.initial_resolved = pc.resolved;
					fcfg.max_solutions = 8;
					Search fs(fd, fa, start_yrp, fcfg);
					fs.RunGuided(target);
					std::printf("  %llu etats en %.1f s, au mieux %u/%zu%s\n",
								(unsigned long long)fs.Stats().nodes,
								fs.Stats().ms / 1000.0,
								fs.Stats().best_overlap, target.codes.size(),
								fs.Stats().exhausted ? "  (EPUISE)" : "");
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
			fa.Shutdown();
		}).join();
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
			// recul, eux, demarrent avant le verrou.
			for(size_t i = 0; i < ents.size() && i < 3; ++i)
				for(uint32_t back : { 15u, 30u })
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
						return;
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
									cfg.max_decisions > pre.size()
										? static_cast<uint32_t>(
											  cfg.max_decisions - pre.size())
										: 64u;
								fcfg.initial_summons = pc.summons;
								fcfg.initial_turns = pc.turns;
								fcfg.initial_resolved = pc.resolved;
								fcfg.max_solutions = opt.optimize ? 12 : 4;
								if(opt.optimize && best_known_burn)
									fcfg.burn_limit = best_known_burn;
								Search fs(fd, fa, start_yrp, fcfg);
								fs.RunLevin(target, plan, merged_policy);
								const SearchStats& st = fs.Stats();
								std::lock_guard<std::mutex> lk(fmx);
								std::printf("  %-14s %9llu exp. %7.1f s  best "
											"%u/%zu  %s%s\n", lbl,
											(unsigned long long)st.nodes,
											st.ms / 1000.0, st.best_overlap,
											target.codes.size(),
											st.exhausted ? "EPUISE"
											: st.hit_time_limit ? "budget"
																: "",
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
							}
							fa.Pop();
						} else {
							std::lock_guard<std::mutex> lk(fmx);
							std::printf("  !! duel de l'approche %zu non "
										"initialisable : %s\n", a, err.c_str());
						}
					}
					fa.Shutdown();
				});
			}
			for(auto& t : apool)
				t.join();
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
				for(const auto& [cell, e] : global_archive)
					if(e.resolves > 0)
						ripped.push_back(&e);
				std::sort(ripped.begin(), ripped.end(),
						  [](const ArchiveEntry* x, const ArchiveEntry* y) {
							  return x->score > y->score;
						  });
				if(ripped.size() > 3)
					ripped.resize(3);
				for(size_t i = 0; i < ripped.size(); ++i)
					for(uint32_t back : { 0u, 20u, 40u })
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
							return;
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
										fcfg.max_decisions =
											cfg.max_decisions > R.pre.size()
												? static_cast<uint32_t>(
													  cfg.max_decisions -
													  R.pre.size())
												: 64u;
										fcfg.initial_summons = pc.summons;
										fcfg.initial_turns = pc.turns;
										fcfg.initial_resolved = pc.resolved;
										fcfg.max_solutions =
											opt.optimize ? 12 : 4;
										if(opt.optimize && best_known_burn)
											fcfg.burn_limit = best_known_burn;
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
										std::lock_guard<std::mutex> lk(fmx);
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
									}
								}
							}
						}
						fa.Shutdown();
					});
				}
				for(auto& t : npool)
					t.join();
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
					return;
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
							SearchConfig fcfg = cfg;
							fcfg.time_limit_ms = left;
							fcfg.max_decisions =
								cfg.max_decisions > roots[i].pre.size()
									? static_cast<uint32_t>(
										  cfg.max_decisions -
										  roots[i].pre.size())
									: 64u;
							fcfg.initial_summons = pc.summons;
							fcfg.initial_turns = pc.turns;
							fcfg.initial_resolved = pc.resolved;
							fcfg.max_solutions = opt.optimize ? 12 : 4;
							if(opt.optimize && best_known_burn)
								fcfg.burn_limit = best_known_burn;
							Search fs(fd, fa, start_yrp, fcfg);
							fs.RunLevin(target, plan, merged_policy);
							const SearchStats& st = fs.Stats();
							std::lock_guard<std::mutex> lk(fmx);
							std::printf("  %-14s %9llu exp. %7.1f s  best %u/%zu"
										"  %s%s\n",
										roots[i].label.c_str(),
										(unsigned long long)st.nodes,
										st.ms / 1000.0, st.best_overlap,
										target.codes.size(),
										st.exhausted ? "EPUISE"
										: st.hit_time_limit ? "budget" : "",
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
						}
						fa.Pop();
					}
				}
				fa.Shutdown();
			});
		}
		for(auto& t : pool)
			t.join();
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
		std::vector<std::atomic<uint32_t>> claims(plan.size() + 1);
		for(auto& c : claims)
			c.store(0, std::memory_order_relaxed);
		// Table de transposition partagee de la passe (lazy SMP).
		std::unique_ptr<SharedTT> stt;
		if(opt.tt_mb && n > 1)
			stt = std::make_unique<SharedTT>(opt.tt_mb);
		std::mutex merge;
		std::vector<Solution> found;
		uint64_t nodes = 0, transpos = 0, cuts = 0;
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
					cuts += s.Stats().novelty_cuts;
					timed_out |= s.Stats().hit_time_limit;
					if(s.Stats().best_overlap > best_overlap) {
						best_board = s.Stats().best_board;
						best_path = s.Stats().best_path;
						best_mzone = s.Stats().best_mzone;
						best_szone = s.Stats().best_szone;
					}
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
		std::printf("  %-8u %10zu %12llu %11llu %10llu %8.1f s   %u/%zu %u mon.%s\n",
					k, found.size(), (unsigned long long)nodes,
					(unsigned long long)transpos, (unsigned long long)cuts,
					ms / 1000.0, best_overlap,
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
			dump("MZONE", ref.target_self.mzone);
			dump("SZONE", ref.target_self.szone);
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
				} else if(synth_ok)
					RunTransplantSolve(duel, *yrp, synth, opt, *arena_ptr,
									   first, db, scripts, patience, cons);
				else if(!opt.deck_file.empty()) {
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
				} else
					RunSolve(duel, *yrp, opt, *arena_ptr, first, db, scripts,
							 patience, cons);
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
