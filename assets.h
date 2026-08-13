// Fourniture des donnees de cartes et des scripts Lua au core.
//
// Reproduit ce que fait EDOPro (data_manager.cpp:100 pour les cartes,
// game.cpp:4120 pour l'ordre de recherche des scripts) sans embarquer le reste
// du client.
#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ocgapi_types.h"

namespace solver {

// Une entree de cards.cdb, mise en forme pour OCG_CardData. Les setcodes sont
// stockes ici car le core conserve le pointeur le temps de la lecture.
struct CardRow {
	uint32_t code{}, alias{}, type{}, level{}, attribute{}, lscale{}, rscale{};
	uint64_t race{};
	int32_t attack{}, defense{};
	uint32_t link_marker{};
	std::vector<uint16_t> setcodes; // termine par 0 si non vide
};

class CardDB {
public:
	// Charge expansions/*.cdb et repositories/**/*.cdb. Un cards.cdb vide a la
	// racine est ignore plutot que de faire echouer le chargement.
	bool Load(const std::string& workdir, std::string& error);

	const CardRow* Find(uint32_t code) const;
	// Nom lisible, pour les rapports uniquement. "?" si inconnu.
	const std::string& Name(uint32_t code) const;
	// Cartes dont le nom contient `needle` (insensible a la casse). Codes
	// CANONIQUES, dedupliques : deux illustrations de la meme carte portent le
	// meme nom et ne doivent compter qu'une fois. Sert a la CLI des
	// contraintes ; un fragment ambigu doit etre signale, pas devine.
	std::vector<std::pair<uint32_t, std::string>>
	FindByName(const std::string& needle) const;
	// Code canonique d'une variante d'illustration. QUERY_OVERLAY_CARD rend le
	// code physique des materiaux (card.cpp:180), sans passer par get_code() :
	// sans cette resolution, deux Xyz aux materiaux d'illustration differente
	// paraissent porter des cartes differentes.
	uint32_t Canonical(uint32_t code) const;
	size_t Size() const { return cards.size(); }
	const std::vector<std::string>& Sources() const { return sources; }

private:
	bool LoadFile(const std::string& path);
	std::unordered_map<uint32_t, CardRow> cards;
	std::unordered_map<uint32_t, std::string> names;
	std::vector<std::string> sources;
};

class ScriptProvider {
public:
	// `override_roots` remplace les depots vivants de l'installation. A utiliser
	// avec un export du depot date de l'epoque du replay : un jeu de scripts
	// decale fait diverger le rejeu en silence (docs/combo-solver-design.md 6bis).
	void Init(const std::string& workdir,
			  const std::vector<std::string>& override_roots);

	// Renvoie le contenu du script, BOM retire. Vide si introuvable.
	// Appelable depuis plusieurs threads : le provider est partage entre les
	// workers, seul le journal des manquants est mutable.
	std::vector<char> Read(const std::string& name);

	const std::vector<std::string>& Dirs() const { return dirs; }
	std::unordered_set<std::string> Misses() const {
		std::lock_guard<std::mutex> lock(misses_mutex);
		return misses;
	}

private:
	std::string workdir;
	std::vector<std::string> dirs;
	mutable std::mutex misses_mutex;
	std::unordered_set<std::string> misses;
};

} // namespace solver
