// Arene memoire instantanable pour un duel ocgcore.
//
// POURQUOI
// L'API OCG n'offre aucun clonage d'etat, et un serialiseur ecrit a la main est
// hors de portee : au moment d'un MSG_SELECT_*, des coroutines Lua sont
// suspendues au milieu d'une resolution d'effet, avec leurs piles et leurs
// upvalues (interpreter.cpp, call_coroutine). On ne sauvegarde donc pas le jeu,
// on sauvegarde LA MEMOIRE.
//
// COMMENT
// Toute la memoire mutable du duel est confinee dans une plage d'adresses que
// l'on controle :
//   - le heap Lua, via l'allocateur passe a lua_newstate (patch luaconf) ;
//   - les objets C++ du core, via la surcharge globale d'operator new/delete.
// La restauration se fait A LA MEME ADRESSE DE BASE, donc tous les pointeurs
// absolus restent valides sans relocation : le duel ne sait pas qu'il a ete
// restaure.
//
// ETAT DE L'IMPLEMENTATION
// L'instantane est une copie complete de la zone servie : simple, sur, et deja
// exploitable au vu du cout mesure d'un pas de moteur (0,18 ms). Le suivi par
// pages sales est branche en INSTRUMENTATION seulement — il mesure combien de
// pages une action salit, chiffre dont depend le passage a une restauration
// incrementale.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace solver {

struct ArenaStats {
	size_t reserved = 0;       // plage d'adresses reservee
	size_t committed = 0;      // reellement engagee aupres de l'OS
	size_t in_use = 0;         // haut de la zone servie (borne du snapshot)
	size_t live_bytes = 0;     // somme des blocs alloues non liberes
	size_t alloc_count = 0;
	size_t free_count = 0;
	size_t host_fallbacks = 0; // allocations sorties de l'arene alors qu'elle
							   // etait active (doit rester a zero)
	bool poisoned = false;     // au moins un repli : l'etat n'est plus capture
};

// Une arene par thread : chaque worker explore son propre sous-arbre, les
// instantanes ne circulent pas entre threads (bases differentes).
class Arena {
public:
	// `reserve` est de l'espace d'adressage, pas de la memoire : seul ce qui est
	// touche est engage. Le jeu de travail doit rester petit pour tenir en L3.
	bool Init(size_t reserve, std::uintptr_t preferred_base, std::string& error);
	void Shutdown();
	bool Ready() const { return base != nullptr; }

	void* Allocate(size_t size);
	void Free(void* ptr);
	void* Reallocate(void* ptr, size_t old_size, size_t new_size);

	// --- EMPOISONNEMENT ------------------------------------------------------
	//
	// Une allocation qui n'a pas tenu dans l'arene part sur le tas de l'hote.
	// `Restore()` ne peut PAS la restaurer : a partir de cet instant l'etat du
	// duel diverge de ce que la recherche croit avoir restaure, et tout ce que
	// ce worker mesure ensuite — noeuds, board keys, solutions — porte sur un
	// duel corrompu. Ce n'est donc pas une statistique, c'est une condition
	// d'arret.
	//
	// Le compteur etait auparavant un `thread_local` lu depuis le thread
	// PRINCIPAL : les replis des workers etaient structurellement invisibles et
	// le rapport imprimait « aucune : tout l'etat est capture » par
	// construction. Il est desormais membre et atomique, et le drapeau est
	// COLLANT (C7).
	void NoteFallback() {
		fallbacks.fetch_add(1, std::memory_order_relaxed);
		poisoned.store(true, std::memory_order_relaxed);
	}
	bool Poisoned() const { return poisoned.load(std::memory_order_relaxed); }
	size_t Fallbacks() const {
		return fallbacks.load(std::memory_order_relaxed);
	}

	bool Contains(const void* p) const {
		auto a = reinterpret_cast<std::uintptr_t>(p);
		return a >= base_addr && a < base_addr + reserved;
	}

	// --- instantanes (pile LIFO, ce qui correspond a un parcours en profondeur)
	//
	// Seules les pages effectivement modifiees sont copiees. Un miroir de
	// l'etat au sommet de pile fournit les images d'avant-modification, que le
	// suivi de pages ne donne pas.
	//
	// Sequence typique d'un DFS :
	//     Push()                      une fois en arrivant sur le noeud
	//     pour chaque fils : avancer, explorer, Restore()
	//     Pop()                       en repartant du noeud
	// Restore() est l'operation frequente, et la moins chere : elle ne recopie
	// que ce que le fils a sali.
	void Push();     // ouvre un niveau : l'etat courant devient le point de reprise
	void Restore();  // revient au point de reprise SANS depiler (iteration sur les fils)
	void Pop();      // revient au point de reprise et depile
	void Discard();  // depile sans restaurer
	size_t Depth() const { return checkpoints.size(); }

	struct CheckpointCost {
		size_t pages = 0;    // pages traitees par la derniere operation
		size_t bytes = 0;
	};
	CheckpointCost LastPush() const { return last_push; }
	CheckpointCost LastRestore() const { return last_restore; }
	size_t UndoBytes() const;   // memoire retenue par la pile de journaux

	// --- instrumentation des pages sales
	void ResetDirtyTracking();
	size_t CountDirtyPages();   // pages ecrites depuis le dernier appel/reset
	bool DirtyTrackingAvailable() const { return write_watch; }

	ArenaStats Stats() const;
	size_t PageSize() const { return page_size; }
	std::uintptr_t BaseAddress() const { return base_addr; }

	// Verifie l'invariant vital : aucune structure de l'allocateur ne doit
	// vivre dans l'arene, sinon une restauration l'ecrase sous ses pieds.
	// Renvoie une description du probleme, vide si tout va bien.
	std::string SelfCheck() const;

	// Allocateur au format attendu par lua_newstate.
	static void* LuaAlloc(void* ud, void* ptr, size_t osize, size_t nsize);

private:
	struct SpanInfo {
		uint8_t klass = 0xff;  // 0xff libre, 0xfe tete de bloc large, 0xfd suite
		uint32_t run = 0;      // nombre de spans, renseigne sur le premier
	};
	// L'etat de l'allocateur fait partie de l'etat a restaurer. Les chainages de
	// blocs libres vivent dans l'arene, donc dans les pages copiees ; les tetes
	// de liste et la table des spans sont gardees hors arene, pour qu'une
	// restauration ne les ecrase pas sous leurs propres pieds.
	struct Checkpoint {
		size_t in_use = 0;
		// Pages modifiees depuis l'empilement de CE niveau. Accumule en
		// logiciel : le suivi materiel n'a qu'un seul jeu de bits global, il ne
		// peut pas servir plusieurs niveaux a la fois.
		std::vector<uint64_t> dirty;
		// Contenu qu'avaient, a l'empilement de ce niveau, les pages modifiees
		// pendant sa periode. Sert a ramener le miroir en arriere quand un fils
		// est depile. Reconstruit a chaque Push d'un fils.
		std::vector<uint32_t> undo_pages;
		std::vector<uint8_t> undo_data;

		std::vector<SpanInfo> spans;
		std::vector<void*> free_lists;
		std::vector<size_t> free_span_runs;
		size_t next_span = 0;
		size_t live_bytes = 0;
	};

	bool CommitTo(size_t offset);
	void* AllocSpans(size_t span_count);
	void FreeSpans(size_t span_index, size_t span_count);
	void EnsureMirror();
	// Verse les bits du suivi materiel dans le niveau au sommet et remet le
	// suivi materiel a zero. Renvoie le nombre de pages du dernier echantillon.
	// TOUTE lecture du suivi materiel doit passer par ici : le lire le remet a
	// zero, donc une lecture qui ne verserait pas les bits les perdrait.
	size_t SyncDirty();
	void CaptureMetadata(Checkpoint& cp) const;
	void RestoreMetadata(const Checkpoint& cp);

	uint8_t* base = nullptr;
	std::uintptr_t base_addr = 0;
	size_t reserved = 0, committed = 0, page_size = 4096;
	bool write_watch = false;

	std::vector<SpanInfo> spans;
	std::vector<void*> free_lists;
	std::vector<size_t> free_span_runs;
	size_t next_span = 0;

	std::vector<Checkpoint> checkpoints;
	// Copie complete de l'etat au sommet de pile. C'est la seule source
	// possible d'images d'avant-modification : le suivi de pages dit QUELLES
	// pages ont change, jamais ce qu'elles contenaient.
	std::vector<uint8_t> mirror;
	size_t live_bytes = 0, alloc_count = 0, free_count = 0;
	// Atomiques : ecrits par le thread proprietaire, lus par lui ET par le
	// thread principal au bilan.
	std::atomic<size_t> fallbacks{ 0 };
	std::atomic<bool> poisoned{ false };
	std::vector<uint8_t*> dirty_scratch;
	CheckpointCost last_push, last_restore;
};

namespace detail {
// Signale une allocation qui a du sortir de l'arene alors qu'elle etait active.
void NoteHostFallback();
} // namespace detail

// Arene ou allouer maintenant (nullptr hors duel ou pendant une pause).
Arena* CurrentArena();
// Arene du thread, independamment des pauses : sert a router les liberations,
// qui peuvent survenir hors de tout scope.
Arena* OwnerArena();

// Rend l'arene active. A poser autour des appels au core et seulement autour
// d'eux : le code hote doit allouer normalement.
class ArenaScope {
public:
	explicit ArenaScope(Arena* a);
	~ArenaScope();
	ArenaScope(const ArenaScope&) = delete;
	ArenaScope& operator=(const ArenaScope&) = delete;

private:
	Arena* previous;
};

// Suspend l'arene a l'interieur d'un ArenaScope. Indispensable dans nos
// callbacks (lecteur de scripts, journal d'erreurs) : ils sont appeles DEPUIS
// le core, donc arene active, mais ce qu'ils allouent appartient a l'hote et ne
// doit pas etre efface par une restauration.
class ArenaPause {
public:
	ArenaPause();
	~ArenaPause();
	ArenaPause(const ArenaPause&) = delete;
	ArenaPause& operator=(const ArenaPause&) = delete;

private:
	Arena* previous;
};

} // namespace solver
