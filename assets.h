// Fourniture des donnees de cartes et des scripts Lua au core.
//
// Reproduit ce que fait EDOPro (data_manager.cpp:100 pour les cartes,
// game.cpp:4120 pour l'ordre de recherche des scripts) sans embarquer le reste
// du client.
#pragma once

#include <cstdint>
#include <mutex>
#include <set>
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
	// PREMIERE LIGNE du texte de carte. Pour les monstres d'extra deck c'est la
	// ligne de MATERIAUX, et son format est regulier :
	//     "Lunalight Leo Dancer" + 3 "Lunalight" monsters
	//     2 Level 4 monsters
	// C'est l'AMORCE du graphe de recettes (chantier 16, regle 3 : « le texte
	// n'est qu'une amorce, la verite vient de l'observation »). Vide si absent.
	const std::string& MaterialLine(uint32_t code) const;
	// Code canonique d'une carte par son nom EXACT (0 si inconnu). Les
	// materiaux nommes dans le texte le sont entre guillemets, exactement.
	uint32_t CodeByExactName(const std::string& name) const;
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

	// Codes que le core a demandes et que la base ne connait pas. C'est le
	// JUMEAU BASE DE DONNEES du decalage de jeux de scripts, et il est plus
	// silencieux que lui : le core donne a la carte inconnue un corps vanille
	// sans effet (duel.cpp), le deck se charge, le duel demarre, la ligne
	// diverge — et il n'existait aucun equivalent de ScriptProvider::Misses()
	// pour le signaler (4.6). Le mutex protege les appels concurrents du
	// lecteur de cartes depuis les workers.
	void NoteUnknown(uint32_t code) const;
	std::vector<uint32_t> UnknownCodes() const;

private:
	bool LoadFile(const std::string& path);
	std::unordered_map<uint32_t, CardRow> cards;
	std::unordered_map<uint32_t, std::string> names;
	// Premiere ligne du texte, par code. Chargee en meme temps que les noms.
	std::unordered_map<uint32_t, std::string> material_lines;
	// Nom exact -> code canonique, construit apres chargement.
	std::unordered_map<std::string, uint32_t> by_name;
	std::vector<std::string> sources;
	mutable std::mutex unknown_mx;
	mutable std::set<uint32_t> unknown;
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
	// Fichiers PRESENTS que la lecture n'a pas rendus (E/S courte, taille nulle).
	// Distincts des manquants : une lecture courte se deguisait en « absent » et
	// faisait charger le MEME script depuis un depot de rang inferieur, donc une
	// autre version — le decalage de jeux de scripts fabrique a partir d'une
	// erreur d'E/S, et invisible parce qu'il n'atteignait jamais `misses` (4.7).
	std::unordered_set<std::string> Unreadable() const {
		std::lock_guard<std::mutex> lock(misses_mutex);
		return unreadable;
	}

private:
	std::string workdir;
	std::vector<std::string> dirs;
	mutable std::mutex misses_mutex;
	std::unordered_set<std::string> misses;
	std::unordered_set<std::string> unreadable;
};

} // namespace solver
