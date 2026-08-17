// LA TABLE D'OPERATEURS DECLARES — lire les cartes au lieu de les observer.
//
// LE FAIT QUI JUSTIFIE CE MODULE (session 19, chantier 0). Sur les 24 cartes du
// deck de l'etalon A, exactement DEUX accordent un etat qui debloque quoi que ce
// soit : `EFFECT_ADD_CODE` (Kaleido Chick) et `EFFECT_EXTRA_FUSION_MATERIAL`
// (Masquerade). Ce sont EXACTEMENT les deux goulots mesures (0,14 % et 0 %,
// 9.27 (c)). Ils se trouvent par un `grep` sur deux constantes du jeu, sans
// nommer une seule carte.
//
// Et le recensement des `CATEGORY_*` NE LES AURAIT PAS TROUVES : il rend
// `CATEGORY_FUSION_SUMMON` une fois (Wolf) et pas un mot des deux pivots. Il
// faut DEUX VOCABULAIRES, et le dossier n'en lisait aucun :
//
//   CATEGORY_*  decrit ce que l'effet fait aux CARTES  (envoyer, chercher)
//   EFFECT_*    decrit quel ETAT il accorde            (renommer, autoriser un
//                                                       materiau du cimetiere)
//
// Le combo repose entierement sur le second. C'EST L'ERREUR DU GRAPHE DE
// RECETTES : il modelise des PRODUITS, jamais des ETATS ACCORDES. `--backward`
// etait le bon algorithme sur un graphe ampute de ses aretes (9.24 (e)) ; les
// aretes manquantes sont les effets, et elles sont DECLAREES.
//
// CE QUE CE MODULE EST, ET CE QU'IL N'EST PAS. C'est une ANALYSE STATIQUE du
// Lua : elle lit la DECLARATION, pas la semantique. Conditions et couts sont des
// fermetures ; leur `chk == 0` n'est pas evalue ici. La table est donc
// OPTIMISTE — elle dit ce qu'une carte declare pouvoir faire, jamais ce qu'elle
// peut faire A CET INSTANT. C'est assez pour un harnais de validation et pour
// alimenter un graphe ; ce n'est pas assez pour decider de la legalite, et le
// core reste seul juge de cela.
//
// AUCUNE CARTE N'EST NOMMEE ICI. Les constantes viennent du `constant.lua` du
// jeu lui-meme, servi par le MEME ScriptProvider que le core : un decalage de
// jeu de scripts decale donc AUSSI la table, au lieu de la faire mentir en
// silence.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "assets.h"

namespace solver {

// --- LES CONSTANTES DU JEU, LUES DANS LE JEU ---------------------------------
//
// `constant.lua` et `archetype_setcode_constants.lua` sont des fichiers du jeu,
// resolus par --scriptdir comme n'importe quel script de carte. Les y lire
// plutot que de les recopier ici a une consequence qui n'est pas cosmetique :
// une constante qui change de valeur entre deux versions de scripts change de
// valeur dans la table, et une constante ABSENTE fait echouer l'evaluation au
// lieu de rendre zero.
class ConstantTable {
public:
	// Rend le nombre de constantes chargees. Les fichiers absents ne sont pas
	// une erreur (un scriptdir minimal peut ne pas les porter) : c'est
	// `Size() == 0` qui doit alerter l'appelant.
	size_t Load(ScriptProvider& sp);

	bool Lookup(const std::string& name, uint64_t& out) const;

	// Evalue une expression de constantes telle qu'elle apparait dans les
	// scripts : `A`, `A+B`, `A|B`, `0x40`, `113`. Rend FAUX si un jeton est
	// inconnu — on ne devine JAMAIS la valeur d'une constante absente, sous
	// peine de fabriquer un operateur qui n'existe pas.
	bool Eval(const std::string& expr, uint64_t& out) const;

	// Nom de la constante de cette famille qui porte exactement cette valeur
	// (chaine vide si aucune). Pour l'IMPRESSION seulement : un masque compose
	// se decompose par ci-dessous.
	std::string NameOf(const std::string& prefix, uint64_t value) const;
	// Decompose un masque en noms de bits de la famille. « LOCATION_MZONE|
	// LOCATION_SZONE » plutot que « 0xc ».
	std::string MaskNames(const std::string& prefix, uint64_t value) const;

	// VALEUR D'UNE CHAINE DE CARTE, telle que `Auxiliary.Stringid` la fabrique.
	//
	// LE DECALAGE EST LU DANS `utility.lua`, PAS SUPPOSE. Le dossier portait
	// « aux.Stringid(id, n) = id * 16 + n » (9.27 (b), et c'est sur cette
	// convention que repose la recuperation du code d'un `MSG_SELECT_YESNO`),
	// alors que les scripts contemporains font `(n & 0xfffff) | code << 20`.
	// Un harnais qui suppose la convention rend « 0 appariee sur 37 » et se lit
	// comme un echec d'EXTRACTION alors que c'est un echec d'HYPOTHESE.
	uint64_t StringId(uint32_t code, uint32_t index) const;
	uint32_t StringShift() const { return string_shift; }
	bool StringShiftRead() const { return string_shift_read; }

	size_t Size() const { return vals.size(); }
	const std::vector<std::string>& Files() const { return files; }

private:
	void Absorb(const std::vector<char>& src);
	void ReadStringId(const std::vector<char>& src);
	std::unordered_map<std::string, uint64_t> vals;
	std::vector<std::pair<std::string, uint64_t>> order;
	std::vector<std::string> files;
	uint32_t string_shift = 20;
	uint64_t string_mask = 0xfffff;
	bool string_shift_read = false;
};

// --- UN EFFET DECLARE --------------------------------------------------------
//
// La forme est reguliere sur l'ensemble des scripts (9.27 (a)) : un
// `Effect.CreateEffect`, une suite de `SetXxx`, un `RegisterEffect`. C'est cette
// regularite — et non une table par carte — qui rend l'extraction generique.
struct DeclaredEffect {
	std::string var;          // e1, e2... (identifiant local, pour le rapport)
	std::string in_function;  // la fonction qui le cree ("initial_effect", ...)
	int line = 0;
	bool registered = false;  // c:RegisterEffect / Duel.RegisterEffect
	// Declare dans `initial_effect` : c'est un OPERATEUR, quelque chose que le
	// joueur peut employer. Declare ailleurs (dans une operation, un cout) :
	// c'est un ETAT ACCORDE, le resultat d'un operateur.
	bool at_init = false;

	uint64_t etype = 0;       // EFFECT_TYPE_* (masque)
	uint64_t range = 0;       // LOCATION_* ou l'effet est UTILISABLE (SetRange)
	uint64_t property = 0;    // EFFECT_FLAG_*
	uint64_t target_range_self = 0, target_range_opp = 0;

	// `SetCode` porte DEUX choses selon le type de l'effet, et c'est exactement
	// la distinction des deux vocabulaires : un declencheur y met un `EVENT_*`
	// (quand), un effet continu y met un `EFFECT_*` (quel etat il accorde). On
	// ne les separe pas par semantique mais par le PREFIXE du jeton — c'est
	// exact, et cela ne suppose rien.
	std::string code_name;
	uint64_t code_value = 0;
	bool code_is_event = false;
	bool code_is_effect = false;

	uint64_t category = 0;    // CATEGORY_* (masque)

	// Description : `aux.Stringid(id, n)` = id * 16 + n, convention universelle
	// des scripts. C'EST L'IDENTITE DU PROMPT — la meme paire (code, desc) que
	// les messages du core transportent, donc la cle d'appariement du harnais.
	bool has_desc = false;
	uint32_t desc_card = 0;   // `id` (0 si la description n'est pas un Stringid)
	uint32_t desc_index = 0;  // `n`
	uint64_t desc_value = 0;  // la valeur complete, telle que le message la porte
	bool desc_is_system = false;   // description numerique (chaine du systeme)

	// Ressource declaree. `SetCountLimit(1)` = une fois par tour et PAR COPIE ;
	// `SetCountLimit(1, id)` = une fois par tour et par NOM. La presence du
	// second argument fait toute la difference, et elle est declaree.
	bool has_count_limit = false;
	uint32_t count_limit = 0;
	bool count_by_name = false;
	std::string count_tag;

	// Fonctions branchees. Leur seule PRESENCE est deja une precondition : un
	// effet avec `SetCost` a un cout a payer, un effet avec `SetCondition` a une
	// garde. Ce que ces fermetures testent n'est pas lisible statiquement.
	std::string fn_cost, fn_condition, fn_target, fn_operation, fn_value;
	// Non vide quand l'operateur n'est PAS declare dans le script de la carte
	// mais par une PROCEDURE du jeu (`Pendulum.AddProcedure`,
	// `Fusion.AddProcSpell`...). Sans cette lecture, la pose d'une echelle
	// Pendule et l'activation d'une Polymerisation n'ont AUCUN operateur
	// declare, et le harnais les compte en echec pour une raison qui n'a rien
	// a voir avec l'extraction.
	std::string from_proc;
};

// Ce qu'un operateur PRODUIT, tel que le script le declare au core par
// `Duel.SetOperationInfo(chain, CATEGORY_x, targets, count, player, LOCATION_y)`.
// C'est la seule declaration de PRODUIT que les scripts portent, et elle donne
// la paire (categorie, zone) que le graphe de recettes n'a jamais eue.
struct DeclaredProduct {
	uint64_t category = 0;
	std::string category_name;
	uint64_t location = 0;
	std::string location_name;
	std::string in_function;   // la fonction qui la declare
	bool possible = false;     // SetPossibleOperationInfo : produit EVENTUEL
};

// Une procedure d'invocation declaree (`Fusion.AddProcMixN`, `Xyz.AddProcedure`,
// ...). C'EST LA RECETTE, en forme machine, et elle est superieure a l'amorce
// par le TEXTE de la carte a deux titres : elle porte les codes et non des noms
// a re-resoudre, et elle ne se trompe jamais de langue.
struct DeclaredRecipe {
	std::string proc;                                    // "Fusion.AddProcMixN"
	std::vector<std::pair<uint32_t, uint32_t>> named;     // (code, compte)
	std::vector<std::pair<uint64_t, uint32_t>> setcode;   // (setcode, compte)
	std::vector<uint32_t> unresolved_counts;             // cardinal non nomme
	bool must_be_fusion_summoned = false;
	int line = 0;
};

// UNE CHAINE DECLAREE, et c'est elle qui porte l'identite des prompts.
//
// LE POINT QUI A FAILLI MANQUER AU HARNAIS. La decision qui ouvre l'acces au
// cimetiere (9.27 (b)) est un `Duel.SelectYesNo(tp, aux.Stringid(id, 2))` : sa
// description n'est PAS un `SetDescription` d'effet, c'est un Stringid pose en
// ligne dans le corps d'une operation. Un harnais qui n'indexerait que les
// `SetDescription` declarerait le PIVOT DU COMBO « non apparie » — c'est-a-dire
// qu'il rendrait un faux negatif sur la seule decision qui compte.
//
// On indexe donc TOUT `aux.Stringid(id, n)` du script, avec l'endroit ou il
// apparait. `site` dit lequel : un prompt d'activation (SetDescription) n'a pas
// les memes preconditions verifiables qu'un prompt pose en cours de resolution.
struct DeclaredString {
	uint64_t value = 0;        // id * 16 + n, tel que le message le transporte
	uint32_t card = 0;
	uint32_t index = 0;
	std::string in_function;
	std::string site;          // "SetDescription", "Duel.SelectYesNo", ...
	std::string effect_var;    // non vide si site == "SetDescription"
	int line = 0;
};

struct CardOperators {
	uint32_t code = 0;
	std::string script;          // nom du fichier resolu (c<code>.lua)
	bool script_found = false;
	std::vector<DeclaredEffect> operators;   // declares dans initial_effect
	std::vector<DeclaredEffect> grants;      // crees en resolution : les ETATS
	std::vector<DeclaredString> strings;
	std::vector<DeclaredRecipe> recipes;
	// Procedures du jeu appelees par `initial_effect` (`Pendulum.AddProcedure`,
	// `Fusion.RegisterSummonEff`, ...). C'est la que vivent les operateurs que
	// le script de la carte ne declare pas lui-meme.
	std::vector<std::string> proc_calls;
	std::vector<DeclaredProduct> products;
	std::vector<uint32_t> listed_names;      // s.listed_names
	std::vector<uint64_t> listed_series;     // s.listed_series
	// Zones que les fonctions de cet effet mentionnent, par fonction. C'est ce
	// qui permet de dire « le cout de cet operateur touche DECK|EXTRA » sans
	// evaluer la fermeture.
	std::unordered_map<std::string, uint64_t> fn_locations;
	// Verbes `Duel.<Verb>` appeles par fonction — la CONSOMMATION declaree.
	std::unordered_map<std::string, std::vector<std::string>> fn_verbs;
	// Archetypes et codes NOMMES par la fonction (`IsSetCard`, `IsCode`). Avec
	// `fn_locations`, c'est ce qui donne une PLACE a une arete negative :
	// « une Lunalight de l'EXTRA » au lieu de « quelque chose dans l'extra ».
	// Sans eux la colonne negative du bilan (9.30) devrait etre inventee.
	std::unordered_map<std::string, std::vector<uint64_t>> fn_setcodes;
	std::unordered_map<std::string, std::vector<uint32_t>> fn_codes;
	// Fonctions locales appelees par une fonction. La contrainte ne vit presque
	// jamais dans la fonction qui detruit : elle vit dans le FILTRE qu'elle
	// passe a `SelectMatchingCard`. Un seul niveau suffit sur ce deck.
	std::unordered_map<std::string, std::vector<std::string>> fn_refs;
};

// LOCATIONS SYMBOLIQUES. `SetRange(LOCATION_PZONE)` vaut 0x200, mais le message
// du core porte la zone PHYSIQUE : une Zone Pendule est une sequence
// particuliere de `LOCATION_SZONE` (0x8). Comparer les deux sans normaliser
// rendrait « precondition VIOLEE » sur toutes les activations de Wolf — un faux
// negatif du harnais, et exactement la meme confusion que celle qui rend
// `--canonical-zones` dangereux (9.27 (e)).
//
// La correspondance est celle du jeu, pas une heuristique : FZONE, PZONE et
// STZONE sont des zones magie ; MMZONE et EMZONE sont des zones monstre.
uint64_t NormalizeRange(uint64_t range);

class OperatorTable {
public:
	// `codes` : les cartes a lire (main + extra + tout code surveille). Rend le
	// nombre de scripts effectivement lus.
	size_t Build(const CardDB& db, ScriptProvider& sp, const ConstantTable& kt,
				 const std::vector<uint32_t>& codes);

	const CardOperators* Find(uint32_t code) const;

	// LA CHAINE QUI PORTE CETTE DESCRIPTION. C'est l'appariement du harnais : le
	// message transporte (code, desc), et `desc = id * 16 + n` designe un et un
	// seul `aux.Stringid(id, n)` du deck. Rend nul si aucun ne correspond.
	const DeclaredString* ByDesc(uint64_t desc) const;
	// L'effet nomme par une chaine de site `SetDescription` (nul sinon).
	const DeclaredEffect* EffectOf(const DeclaredString& s) const;
	// Repli quand la description n'est pas un Stringid (chaine systeme) : le
	// seul operateur ACTIVABLE de cette carte qui n'a pas de description propre.
	// Rend nul des qu'il y a ambiguite — on ne devine pas.
	const DeclaredEffect* SoleUndescribed(uint32_t code, uint64_t act_mask) const;
	// Masque EFFECT_TYPE_* des effets qu'un joueur peut EMPLOYER (les operateurs
	// au sens strict). Compose depuis `constant.lua` : aucune valeur en dur.
	static uint64_t ActivatableMask(const ConstantTable& kt);

	void Print(const CardDB& db, const ConstantTable& kt) const;
	// Le recensement qui a designe le chantier : quelles cartes accordent un
	// ETAT, et lequel. C'est la ligne qui rend les deux pivots.
	void PrintGrants(const CardDB& db, const ConstantTable& kt) const;

	// LA COLONNE NEGATIVE — ce qu'un operateur DETRUIT (session 20, chantier B).
	//
	// Le graphe savait dire ce qu'un operateur EXIGE ; il ne savait pas dire ce
	// qu'il DETRUIT. La matiere etait extraite depuis la s19 (`fn_verbs`,
	// `fn_locations`, `count_limit`) et lue par PERSONNE. Ce rapport la lit.
	//
	// CE N'EST PAS UNE AMELIORATION, C'EST UNE CONDITION DE CORRECTION :
	// 9.28 (h) a mesure que DOUZE aretes d'acquisition sur treize decrivaient
	// des routes qui DETRUISENT le but, et le critere qui les a retirees etait
	// un pis-aller. Le vrai critere est la consommation, et il se calcule.
	//
	// `goal_zone` est la zone dont on veut le bilan (LOCATION_EXTRA pour un but
	// en monstres d'extra deck) : la synthese finale n'y garde que les aretes
	// qui consomment DANS cette zone — c'est la ligne `A_p` de l'equation de
	// bilan, celle dont la faisabilite decide qu'un tirage est mort.
	void PrintConsumption(const CardDB& db, const ConstantTable& kt,
						  uint64_t goal_zone) const;

	// MARCHE 1 — LES COMPTES DE TIR, CONFRONTES AUX CAPACITES.
	//
	// L'equation de bilan rend trois choses ; la troisieme seulement est une
	// heuristique. Les deux premieres sont ici : le vecteur `x` (quel operateur,
	// et COMBIEN DE FOIS) et la FAISABILITE (`x_o <= cap_o` ?). Un but a trois
	// exemplaires identiques ne demande pas « la recette de Liger » : il demande
	// que l'invocation tire TROIS FOIS, et que chaque exigence soit servie trois
	// fois. C'est la multiplicite que `RecipeDistance` n'a jamais portee
	// (« 2 "Nom" » y est pose comme deux exigences d'une copie) et que le biais
	// d'operateur ne sait pas designer — il ne nomme qu'UNE carte.
	//
	// PORTEE, DITE D'AVANCE. C'est une expansion ET, par la recette DECLAREE :
	// exacte quand la recette est unique (le cas de Liger), et seulement
	// NECESSAIRE quand plusieurs voies existent — on ne choisit pas a la place
	// du jeu, on developpe la voie declaree et on dit qu'on l'a fait. Ni l'ordre
	// ni la legalite n'y entrent : les conditions sont des fermetures.
	//
	// `deck` porte les codes AVEC leurs doublons (c'est le nombre de copies qui
	// decide d'une capacite « par COPIE ») ; `goal` est (code, exemplaires).
	void PrintFiringCounts(
		const CardDB& db, const ConstantTable& kt,
		const std::vector<uint32_t>& deck,
		const std::vector<std::pair<uint32_t, uint32_t>>& goal) const;

	size_t Cards() const { return cards.size(); }
	size_t Missing() const { return missing; }
	size_t OperatorCount() const;
	size_t GrantCount() const;
	size_t ProductCount() const;

	const std::unordered_map<uint32_t, CardOperators>& All() const { return cards; }

private:
	std::unordered_map<uint32_t, CardOperators> cards;
	// desc complete -> (code de carte, index dans `strings`)
	std::unordered_map<uint64_t, std::pair<uint32_t, size_t>> by_desc;
	size_t missing = 0;
};

// --- LE PROGRAMME D'OPERATEURS (9.30) ---------------------------------------
//
//     h(s) = min c'x   s.c.  A'x >= M_G - M_s ,  0 <= x <= u
//
// Quatre proprietes DEMONTREES en 9.30 : admissibilite (th. 1), consistance —
// donc gradient — (th. 2), impasses prouvees par infaisabilite (th. 3), et
// resserrement libre par toute contrainte que tout plan satisfait (th. 4).
//
// Aucune des quatre ne vaut si le SOLVEUR ment. C'est le seul point non
// demontrable du chantier, et il est traite comme tel : `LPResult` porte ses
// propres gardes, verifiees a chaque appel, et le module s'auto-teste sur des
// instances a solution connue avant de servir.
struct OperatorLP {
	size_t n_ops = 0;
	std::vector<double> cost;    // c
	std::vector<double> upper;   // u ; kNoBound = sans borne
	struct Row {                 // sum coef.x >= rhs
		std::vector<std::pair<size_t, double>> coef;
		double rhs = 0.0;
		std::string label;
	};
	std::vector<Row> rows;
	static constexpr double kNoBound = 1e18;
};

struct LPResult {
	bool feasible = false;
	double value = 0.0;
	std::vector<double> x;
	// GARDES. `primal_ok` verifie A'x >= b et 0 <= x <= u sur la solution
	// rendue ; `optimal_ok` verifie que plus aucun cout reduit n'est negatif.
	// Un `false` ici invalide la mesure AVANT qu'elle serve, au lieu de la
	// laisser passer pour une heuristique « un peu optimiste ».
	bool primal_ok = false;
	bool optimal_ok = false;
	double worst_violation = 0.0;
};

LPResult SolveOperatorLP(const OperatorLP& lp);

// Assemble `A`, `b` et `u` depuis la table, le deck et le but, puis imprime
// `h` et le vecteur de tirs `x`. Rend `h` (ou -1 si infaisable, th. 3).
//
// TROIS ZONES ABSTRAITES, ET PAS UNE DE PLUS. `RESERVE` (deck + extra),
// `DISPO` (main, terrain, cimetiere, bannie — tout ce qui peut servir de
// materiau) et `TERRAIN` (le but s'y lit). Une zone de plus serait une
// hypothese de plus a justifier.
//
// LA REGLE QUI GOUVERNE CHAQUE CHOIX DE MODELISATION : au moindre doute, on
// SOUS-CONTRAINT. Sous-contraindre garde `h <= h*` (theoreme 1 tient, donc
// `h = infini` reste une PREUVE d'impasse) ; sur-contraindre rendrait `h` plus
// grand que le vrai cout et transformerait la preuve en mensonge. C'est
// l'asymetrie de 9.29 (f), appliquee au modele entier.
class BalanceModel {
public:
	bool Build(const OperatorTable& tbl, const CardDB& db,
			   const ConstantTable& kt, const std::vector<uint32_t>& deck,
			   const std::vector<std::pair<uint32_t, uint32_t>>& goal);
	// `res` (deck+extra), `ava` (main, terrain, cimetiere, bannie) et `fld`
	// (zone monstre) sont les codes PHYSIQUES presents dans chaque zone a
	// l'etat courant. Rend h, ou -1 si infaisable (theoreme 3).
	double Solve(const std::vector<uint32_t>& res,
				 const std::vector<uint32_t>& ava,
				 const std::vector<uint32_t>& fld,
				 LPResult* out = nullptr) const;
	// LES SOUS-BUTS, DERIVES DE x*. Chaque transition qui tire produit des
	// places : ce sont les etapes que tout plan optimal du programme doit
	// franchir. C'est la SERIALISATION, calculee et non devinee — et elle porte
	// les places INTERMEDIAIRES (un corps disponible, un code acquis), la ou
	// `CommonCodes` ne compte que les cartes cibles POSEES et reste donc plat
	// sur toute la montee (9.29 (k) : 85 % d'etats muets).
	struct Need {
		uint32_t code = 0;    // 0 si l'exigence est un archetype
		uint64_t arch = 0;
		uint8_t zone = 0;     // 0 RESERVE, 1 DISPO, 2 TERRAIN, 3 CIMETIERE
		uint32_t count = 1;
	};
	std::vector<Need> NeedsFrom(const LPResult& r) const;
	// LES BARREAUX DE CONSOMMATION (s21). Le profil des ecarts a montre trois
	// deserts de 46 a 73 reponses sur la ligne reelle : tout le travail
	// d'ASSEMBLAGE (renommages, fusions) y est invisible parce que ses produits
	// retombent dans des places agregees deja saturees — la forme close
	// (cout ~ Sigma b^(l_i), domine par b^(l_max)) dit que ces deserts seuls
	// interdisent le run nu. Or chaque tir consommateur ENVOIE un corps au
	// cimetiere, et cette arrivee-la monte REGULIEREMENT pendant les deserts.
	// On rend donc la colonne NEGATIVE de x* en sous-buts @CIMETIERE :
	// « combien de tirs exiges ont eu lieu », lu dans l'etat — le critere de
	// progres que 9.31 nommait sans l'avoir construit. Sur-compter est ANODIN
	// (une unite jamais atteinte ne cree pas de cellule) ; sous-compter
	// laisserait les deserts entiers.
	std::vector<Need> ConsumedFrom(const LPResult& r) const;

	size_t Places() const { return pname.size(); }
	size_t Transitions() const { return lp.n_ops; }
	size_t Renames() const { return n_rename; }
	const std::vector<std::string>& TrNames() const { return tname; }
	const std::vector<std::string>& PlaceNames() const { return pname; }
	// Nom de la premiere place PRODUITE par cette transition (lisibilite).
	std::string Produces(size_t t) const;

private:
	size_t PlaceId(int kind, uint64_t key, int zone) const;
	std::vector<uint64_t> SetcodesOf(uint32_t code) const;
	const CardDB* db = nullptr;
	std::unordered_map<uint64_t, size_t> pid;
	std::vector<std::string> pname;
	std::vector<std::string> tname;
	std::vector<double> need;                       // but par place
	std::vector<std::unordered_map<size_t, double>> col;   // effets par transition
	OperatorLP lp;                                  // couts et bornes ; rows rebati
	// Codes canoniques du BUT : les barreaux de consommation ne doivent jamais
	// porter sur eux (s21 — l'echelle recompensait l'envoi des LIGERS au
	// cimetiere, un artefact du descost dans x*, et le run nu archivait des
	// cellules « en progres » qui avaient detruit leurs pieces de but).
	std::vector<uint32_t> goal_codes;
	size_t n_rename = 0;
	mutable std::unordered_map<uint32_t, std::vector<uint64_t>> sc_cache;
};

double BuildAndSolveBalance(const OperatorTable& tbl, const CardDB& db,
							const ConstantTable& kt,
							const std::vector<uint32_t>& deck,
							const std::vector<std::pair<uint32_t, uint32_t>>& goal);

// Rend le nombre de cas passes sur le nombre de cas. Doit valoir n/n.
size_t SelfTestOperatorLP(size_t* total);

// Extraction d'UN script deja lu. Exposee pour le test : elle ne touche ni au
// disque ni a la base.
//
// `init_fn` nomme la fonction dont les effets sont des OPERATEURS. C'est
// `initial_effect` pour un script de carte ; pour un fichier `proc_*.lua` c'est
// la procedure appelee (`Pendulum.AddProcedure`), et les effets qu'elle
// enregistre appartiennent alors a la carte appelante.
CardOperators ParseScript(uint32_t code, const std::vector<char>& src,
						  const ConstantTable& kt,
						  const std::string& init_fn = "initial_effect");

// --- LE TYPE DE NŒUD MANQUANT (chantier 1, session 19) -----------------------
//
// Le graphe de recettes range `Lunalight Leo Dancer` comme un PRODUIT A
// FABRIQUER — d'ou `--backward` et ses « 2 sous-produits, 0,02 fabrique »
// (9.24 (e)) : il essayait de construire une carte non constructible, puisque
// son materiau nomme est absent du deck.
//
// Or Leo n'est pas ici un produit : c'est une PROPRIETE ACQUERABLE. `Kaleido
// Chick` accorde `EFFECT_ADD_CODE` — le code de la carte qu'elle envoie au
// cimetiere, valable comme MATERIAU DE FUSION. Le coup EST dans l'espace ; il
// demande une preparation en deux temps, et rien dans le graphe ne pouvait
// l'exprimer.
//
// Une arete d'acquisition, c'est donc : « ce CODE s'obtient si l'HOTE est dans
// sa zone et si une SOURCE portant ce code est dans la zone que l'operateur
// atteint ». Deux exigences, aucune fabrication.
struct AcquirableCode {
	uint32_t code = 0;         // le code acquis
	uint32_t host = 0;         // la carte qui porte l'operateur
	uint64_t host_range = 0;   // ou elle doit etre (LOCATION_*, deja normalise)
	uint64_t source_zone = 0;  // ou la source doit etre
	std::string grant;         // EFFECT_ADD_CODE / EFFECT_CHANGE_CODE
};

// `owned` : les cartes que le deck possede (main + extra). L'arete n'est posee
// que vers des codes REELLEMENT disponibles — une acquisition vers un code
// absent serait une route morte, exactement le piege 63 du graphe de recettes.
std::vector<AcquirableCode> AcquirableCodesOf(const OperatorTable& tbl,
											  const CardDB& db,
											  const std::vector<uint32_t>& owned);

// --- LE HARNAIS : CONFRONTER LA TABLE AU PLAN RESOLU -------------------------
//
// Une activation relevee dans le rejeu. Le decodeur des messages la rend ;
// `operators.cpp` ne decode rien lui-meme.
struct ObservedActivation {
	size_t at = 0;            // index de reponse
	uint8_t message = 0;      // MSG_SELECT_IDLECMD, _CHAIN, _EFFECTYN, ...
	uint32_t code = 0;        // carte engagee (canonique)
	uint64_t desc = 0;
	uint8_t location = 0;     // zone PHYSIQUE d'ou la carte s'active
	uint32_t sequence = 0;
	int turn = 0;
};

struct HarnessVerdict {
	size_t total = 0;
	// La description est un `aux.Stringid` d'une carte du deck : elle designe UN
	// operateur et un seul. C'est le cas le plus fort.
	size_t matched_by_desc = 0;
	// La description est une chaine SYSTEME que l'operateur declare lui-meme
	// (`SetDescription(1160)` d'une procedure) : l'appariement reste exact.
	size_t matched_by_system = 0;
	// La description est fabriquee par le CORE (`processor.cpp` emet 221 ou 0
	// pour « activer l'effet declencheur de cette carte ? »). Le message ne dit
	// alors PAS quel effet : seule la carte identifie l'operateur. Ce compteur
	// mesure donc, en creux, ce que l'identite (code, description) NE separe
	// PAS — et c'est une propriete du protocole, pas de l'extraction.
	size_t matched_by_card = 0;
	// ... et parmi celles-ci, celles ou plusieurs operateurs restent possibles
	// meme apres la zone. Le harnais ne tranche pas : il compte.
	size_t ambiguous = 0;
	size_t unmatched = 0;            // AUCUN operateur declare : la table ment
	size_t no_script = 0;            // carte sans script lu (hors table)
	size_t zone_checked = 0, zone_ok = 0, zone_violated = 0;
	size_t count_checked = 0, count_violated = 0;
	std::vector<std::string> failures;   // le detail, borne
};

HarnessVerdict ConfrontPlan(const OperatorTable& tbl, const CardDB& db,
							const ConstantTable& kt,
							const std::vector<ObservedActivation>& acts);
void PrintVerdict(const HarnessVerdict& v);

} // namespace solver
