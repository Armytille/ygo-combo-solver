// Enveloppe autour d'un duel ocgcore lie statiquement.
//
// Le solveur n'utilise que l'API publique (ocgapi.h) : aucune dependance au
// rendu, aucune dependance a gframe.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "arena.h"
#include "assets.h"
#include "ocgapi.h"
#include "ocgapi_constants.h"
#include "ocgapi_types.h"
#include "replay.h"

namespace solver {

struct Message {
	uint8_t type{};
	const uint8_t* data{};   // pointe dans le tampon du core, valide jusqu'au
	uint32_t size{};         // prochain Process()
};

// Une carte telle que la renvoie OCG_DuelQueryLocation.
struct QueriedCard {
	bool present{ false };   // false = emplacement vide
	uint32_t code{}, alias{}, position{}, type{}, level{}, status{};
	int32_t attack{}, defense{};
	uint32_t link{}, link_marker{};
	std::vector<uint32_t> overlay;
	std::vector<uint32_t> counters;

	// QUERY_CODE rend le code imprime ; QUERY_ALIAS rend card::get_code()
	// (card.cpp:236), qui resout les variantes d'illustration — 29053657 et
	// 29053656 sont le meme Quetzacoatl — et les effets de changement de nom.
	//
	// Comparer deux boards doit passer par ICI : deux decks qui jouent des
	// illustrations differentes de la meme carte produisent le meme terrain, et
	// les distinguer rendrait la cible artificiellement inatteignable.
	// `code` reste le code physique, seul valide pour compter les exemplaires
	// consommes dans un deck.
	uint32_t Code() const { return alias ? alias : code; }
};

class Duel {
public:
	// `arena` peut etre nul : le duel tourne alors sur l'allocateur systeme,
	// sans instantane possible. Sinon TOUTE la memoire du duel y est confinee.
	Duel(CardDB& db, ScriptProvider& scripts, Arena* arena = nullptr);
	~Duel();
	Duel(const Duel&) = delete;
	Duel& operator=(const Duel&) = delete;

	// Cree le duel et charge constant.lua / utility.lua comme le fait
	// Game::SetupDuel (game.cpp:4173).
	bool Create(const uint64_t seed[4], uint64_t flags, uint32_t lp,
				uint32_t hand, uint32_t draw, std::string& error);

	// Met en place decks et extra puis demarre. Reproduit
	// ReplayMode::StartDuel (old_replay_mode.cpp:108), mode hand test compris.
	// `extra_hand` : cartes AJOUTEES a la main de `extra_hand_team` avant le
	// demarrage (--opp-hand). C'est ce qui donne une vraie main a l'adversaire
	// d'un hand test : sans cartes jouables en face, le core n'ouvre aucune
	// fenetre de reponse adverse — la garde serait satisfaite par vacuite et le
	// handrip ne ripperait rien.
	bool Setup(const Replay& yrp, std::string& error,
			   const std::vector<uint32_t>* extra_hand = nullptr,
			   uint8_t extra_hand_team = 1);

	int Process();                       // OCG_DUEL_STATUS_*
	std::vector<Message> Messages();     // messages produits par le dernier Process
	// Variante sans allocation apres echauffement : `out` est reutilise.
	void Messages(std::vector<Message>& out);
	void SetResponse(const std::vector<uint8_t>& data);

	std::vector<QueriedCard> Query(uint8_t con, uint32_t loc, uint32_t flags);
	// Variante reutilisant `out` (les chemins chauds interrogent les zones a
	// chaque decision : l'allocation par requete etait un point chaud mesure).
	void Query(uint8_t con, uint32_t loc, uint32_t flags,
			   std::vector<QueriedCard>& out);
	// Codes seuls (Code() = alias sinon code), zones cachees des atomes et de
	// la garde : pas de QueriedCard du tout.
	void QueryCodes(uint8_t con, uint32_t loc, std::vector<uint32_t>& out);
	uint32_t Count(uint8_t team, uint32_t loc);

	// Etat que les zones ne montrent pas : pile de resolution en cours, chaine
	// courante, compteurs "une fois par tour", phase, LP. Sans lui, deux
	// instants distincts d'une meme resolution ont le meme digest et la
	// recherche elague la branche du combo (patch C1).
	const std::vector<uint8_t>& ProcessorState();

	bool LoadScript(const std::string& name);
	bool ExecLua(const std::string& code, const std::string& chunk_name = " ");

	// Arrete ou relance le ramasse-miettes Lua. Arrete, son marquage cesse
	// d'ecrire dans l'en-tete de tous les objets vivants — c'est ce qui salit
	// massivement les pages et ruine la restauration incrementale. La memoire
	// n'est pas perdue pour autant : une restauration recupere tout ce qu'une
	// branche abandonnee a alloue. Sans effet sur les regles : ocgcore ne
	// s'appuie sur aucun metamethode __gc, il compte ses references lui-meme.
	bool SetLuaGc(bool enabled);

	const std::vector<std::string>& Errors() const { return errors; }
	OCG_Duel Handle() const { return handle; }
	Arena* GetArena() const { return arena; }
	const CardDB& Db() const { return db; }

private:
	static void CardReaderThunk(void* payload, uint32_t code, OCG_CardData* data);
	static int ScriptReaderThunk(void* payload, OCG_Duel duel, const char* name);
	static void LogThunk(void* payload, const char* msg, int type);

	CardDB& db;
	ScriptProvider& scripts;
	Arena* arena{ nullptr };
	OCG_Duel handle{ nullptr };
	std::vector<std::string> errors;
	std::vector<uint8_t> processor_state;
	// Requetes d'etat de processeur rendues VIDES par le core. Non nul, le
	// digest perd sa composante de chaine : deux instants distincts d'une meme
	// resolution se confondent et la branche du combo est elaguee des le debut
	// — le patch C1 defait en silence (audit 18).
	size_t empty_processor_states = 0;

public:
	size_t EmptyProcessorStates() const { return empty_processor_states; }
};

// Decoupe le tampon renvoye par OCG_DuelQueryLocation (prefixe par sa taille
// utile, ocgapi.cpp:234) en cartes.
std::vector<QueriedCard> ParseQueryStream(const uint8_t* data, uint32_t len);
void ParseQueryStreamInto(const uint8_t* data, uint32_t len,
						  std::vector<QueriedCard>& out);

} // namespace solver
