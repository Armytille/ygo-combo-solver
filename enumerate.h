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
	// Description de l'effet engage (s22ter) — la meme valeur u64 que le
	// message transporte (aux.Stringid). 0 hors des fenetres de chaine.
	// C'est elle qui donne a --no-self-negate sa precision d'EFFET : une
	// carte peut porter une negation ET un autre effet rapide.
	uint64_t desc = 0;
	// L'IDENTITE CI-DESSUS EST APPROXIMATIVE sur les prompts de SELECTION sous
	// `card_on_select` : un sous-ensemble de k > 1 cartes n'a pas d'identite, et
	// l'on retient arbitrairement son premier code. Le VOLET `*_sel` de
	// `hint_seen` (search.h) mesure exactement la part du biais qui porte sur ces
	// identites-la — c'est la ventilation, et non un drapeau, qui separe les deux.
	//
	// UN CHAMP `card_lossy` A EXISTE ICI (session 17) pour couper le biais
	// d'indices sur ces prompts. Il est SUPPRIME (audit 18), et il faut dire
	// pourquoi plutot que de l'effacer : il n'a JAMAIS ete assigne `true` — ni
	// dans `EnumerateRaw`, ni ailleurs — donc le garde de `search.cpp` etait
	// inerte et son commentaire decrivait un comportement inexistant. La mesure
	// qui le justifiait (« l'etalon B tombe de 2 239 a ZERO ») est elle-meme
	// retiree : le juge de cet etalon rend 0, 0, 0, 89, 2 239 sur cinq
	// executions de la MEME commande a la MEME graine (9.25 (b)). Il n'y avait
	// donc ni effet a couper, ni coupure.
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
	// Retire le choix i (s22ter, --no-self-negate) : compactage en place, la
	// derniere option (decliner) garde sa place de derniere.
	void RemoveAt(size_t i) {
		for(size_t j = i; j + 1 < used; ++j)
			store[j] = std::move(store[j + 1]);
		if(used)
			--used;
	}
	Choice& Emit() {
		if(used == store.size())
			store.emplace_back();
		Choice& c = store[used++];
		c.response.clear();
		c.label.clear();
		c.edge = 0;
		c.plan_key = 0;
		c.card = 0;
		c.desc = 0;
		// LE POOL REUTILISE SES ELEMENTS : tout champ ajoute a `Choice` doit etre
		// remis a zero ici, sous peine de fuiter d'une decision a l'autre. La
		// leçon vient d'un defaut reel (session 17) : un drapeau non reinitialise
		// coupait le biais d'indices PARTOUT et le cas s'effondrait en silence.
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
	// Plafonne le NOMBRE de sous-ensembles enumeres (SELECT_CARD, SUM...).
	// Au-dela, on echantillonne les tailles extremes plutot que d'exploser.
	// C'est ce nombre qui plafonne le facteur de branchement de TOUS les
	// prompts de selection ; il etait ecrit en dur (24) a douze endroits, ce
	// defaut de 64 n'etant jamais utilise. Voir --max-subsets (C16).
	uint32_t max_subsets = 64;
	// Incremente quand le plafond ci-dessus a coupe une enumeration : sans lui
	// une selection tronquee est indiscernable d'une selection complete, et une
	// preuve d'absence portant sur un prompt de somme n'en est plus une (C9).
	// Non nul, il pointe un compteur propre au Search appelant.
	uint64_t* subsets_capped = nullptr;
	// `subsets_ascending` (ordre HISTORIQUE, tailles croissantes) a vecu ici.
	// SUPPRIME (audit 18) : son unique role etait de rejouer l'A/B d'attribution
	// du correctif C9, et cet A/B est TERMINE. L'ordre retenu alterne depuis les
	// deux bouts ; l'ordre croissant, a cap = 24 sur 24 candidats, n'emettait que
	// des singletons.
	// N'explore qu'une zone libre representative par type de zone. Faux par
	// defaut : les fleches de lien et les colonnes peuvent tout changer.
	bool canonical_zones = false;
	// L'IDENTITE DE CARTE SUR LES PROMPTS DE SELECTION EST DESORMAIS
	// INCONDITIONNELLE (session 18ter). Elle a vecu derriere `--card-on-select`
	// pendant trois sessions, eteinte par defaut — et son absence rendait
	// STRUCTURELLEMENT NULS les compteurs de la sonde sur ces prompts, ce qui
	// s'imprimait « JAMAIS RETENUE » et se lisait comme un fait.
	//
	// CE QUI JUSTIFIE DE NE PLUS EN FAIRE UN DRAPEAU : renseigner `Choice::card`
	// n'est PAS une preference, c'est lire une information que le message porte
	// deja. Ce qui etait discutable n'a jamais ete l'identite, c'est le BIAIS
	// D'INDICES qui s'y appliquait — et c'est LUI qui est desormais garde, par
	// `IsSubsetPrompt` dans search.cpp. C'est ce que `Choice::card_lossy` devait
	// faire en session 17 et n'a jamais fait, faute d'avoir ete branche.
	// L'IDENTITE DES PROMPTS OUI/NON EST INCONDITIONNELLE, pour la meme raison :
	// un prompt qui porte son code et sa description dans le message n'a aucune
	// raison de les jeter. Sans elle, TOUS les « oui » de la partie partageaient
	// UN SEUL poids de politique.
	// SONDE D'OFFRE (session 17) — l'instrument qui DECOMPOSE la loi d'arite.
	//
	// La session 16 a mesure que la frequence d'une invocation s'effondre avec
	// son nombre de materiaux, sans pouvoir dire OU. Deux pannes opposees
	// produisent le meme zero :
	//   - la Fusion n'est JAMAIS PROPOSEE — le core ne la liste que si ses
	//     materiaux sont payables A CET INSTANT : la panne est dans l'ETAT, donc
	//     dans le `h` ou dans la decomposition ;
	//   - elle est PROPOSEE et jamais prise — la panne est dans
	//     l'ECHANTILLONNAGE (troncature des sous-ensembles, poids de politique).
	// `--goal-bias` (session 16) a echoue precisement faute de cette lecture.
	//
	// `watch` : codes canoniques surveilles (au plus 4, meme liste que
	// SearchConfig::probe_watch). `watch_offered` : un bit par code, POSE des que
	// le code apparait dans le pool d'un prompt. STRICTEMENT observationnel —
	// aucun choix ajoute, retire ni repondere. Nul = cout d'un test de pointeur.
	const std::vector<uint32_t>* watch = nullptr;
	uint64_t* watch_offered = nullptr;
	// ASSIGNATION RESOLUE (session 17, chantier 1 ; Delarue et al.,
	// arXiv:2010.12001 — la selection d'action posee en OPTIMISATION au lieu
	// d'etre enumeree puis echantillonnee).
	//
	// LE DEFAUT QU'ELLE CORRIGE. `max_subsets` plafonne l'enumeration : a 64
	// sous-ensembles pour C(n,k) possibles, le sous-ensemble qui sert le plan
	// n'est PAS ARBITRAIREMENT rare, il est ABSENT — la troncature est
	// lexicographique, pas aleatoire. Aucun poids de politique ne peut rattraper
	// un choix qui n'est jamais emis.
	//
	// CE QUE C'EST. Codes CANONIQUES qui servent une exigence d'une recette
	// d'une carte cible manquante, calcules par le Search depuis le graphe de
	// recettes — l'enumerateur reste ignorant du but, il ne recoit qu'une liste.
	// Non nul : les prompts de SOUS-ENSEMBLE emettent EN PLUS les deux
	// sous-ensembles EXTREMES au sens de ce compte (le plus utile et le moins
	// utile), dedoublonnes contre ce qui a deja ete emis.
	//
	// LES DEUX EXTREMES ET PAS UN SEUL, et c'est le point honnete : un prompt de
	// selection ne dit pas s'il demande des materiaux (on en veut d'utiles) ou
	// des cartes a defausser (on en veut d'inutiles). On ne DECIDE donc pas du
	// signe : on garantit que les deux sous-ensembles informatifs sont dans le
	// pool, et la politique apprend lequel, par plan_key. Regle 2 du chantier 16 :
	// le mecanisme n'enleve jamais rien de l'espace.
	const std::vector<uint32_t>* assign_useful = nullptr;
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
	// CONTRAINTE MP1 SEULE (s22quater, demande operateur) : l'entree en
	// Battle Phase est retiree de l'enumeration (la Main 2 n'existe qu'apres
	// la BP — la retirer aussi n'est que ceinture pour les prefixes
	// enregistres avant la contrainte). « -> End Phase » reste toujours : le
	// tour doit se fermer, et retirer la derniere reponse legale serait une
	// corruption de l'espace, pas une contrainte.
	bool mp1_only = false;
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
//
// TRI-ETAT (C10). L'ancienne version rendait `false` = AUTORISE sur tous ses
// chemins d'echec, alors qu'elle decode une disposition de message ocgcore
// codee en dur : une derive de format faisait cesser --no-activate et
// --no-chain de filtrer, EN SILENCE, tout en restant crus.
enum class Verdict { Allowed, Forbidden, Undecodable };
Verdict ResponseVerdict(uint8_t message, const uint8_t* data, uint32_t len,
						const std::vector<uint8_t>& response,
						const EnumOptions& opt);

// Enveloppe conservatrice : `Undecodable` compte comme INTERDIT — la branche
// est retiree plutot qu'admise sans controle — et le cas est compte.
bool ResponseForbidden(uint8_t message, const uint8_t* data, uint32_t len,
					   const std::vector<uint8_t>& response,
					   const EnumOptions& opt);

// Nombre de reponses que le decodeur n'a pas su lire. NON NUL = la disposition
// des messages du core a derive : les filtres ne veulent plus rien dire et le
// run doit etre jete. Imprime au bilan.
uint64_t UndecodableResponses();

// --- CE QU'UNE REPONSE ENREGISTREE ACTIVE (session 19, harnais d'operateurs) --
//
// Le harnais de validation confronte la TABLE D'OPERATEURS extraite des scripts
// a un plan RESOLU : chaque activation de la ligne correspond-elle a un
// operateur declare, et sous quelle zone ? La paire (code, description) est
// l'identite dont la session 18ter a dote les prompts ; ici on la relit depuis
// la reponse ENREGISTREE, ce que `Enumerate` ne fait pas (il enumere ce qui est
// possible, pas ce qui a ete joue).
//
// La zone est la piece qui manquait : `SetRange` declare ou l'effet est
// utilisable, et le message porte la zone PHYSIQUE d'ou la carte s'active.
// Les confronter est la seule precondition qu'une analyse statique puisse
// verifier sans evaluer une fermeture.
struct ActivationRead {
	uint32_t code = 0;
	uint64_t desc = 0;
	uint8_t location = 0;    // 0 = le message ne porte pas de zone
	uint32_t sequence = 0;
	bool has_zone = false;
};

// Rend faux si la reponse n'active rien (invocation, changement de phase, « ne
// pas chainer », prompt de selection) OU si le message ne se decode pas. Les
// deux cas se distinguent par `UndecodableResponses()`, qui compte le second.
bool DecodeActivation(uint8_t message, const uint8_t* data, uint32_t len,
					  const std::vector<uint8_t>& response,
					  const EnumOptions& opt, ActivationRead& out);

} // namespace solver
