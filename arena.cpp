#include "arena.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#if defined(__EMSCRIPTEN__)
// --- DOS WEBASSEMBLY -------------------------------------------------------
//
// Trois choses manquent, et une seule est difficile.
//
//   1. VirtualAlloc/VirtualFree. La memoire lineaire wasm est UN bloc contigu
//      qui ne bouge jamais : un malloc unique au demarrage satisfait, gratis,
//      l'invariant central de l'arene (« restaurer a la MEME adresse de base »).
//      La reserve paresseuse disparait — `reserve` devient du commit reel, il
//      faut donc le dimensionner au filigrane observe (--arena-mb), pas a
//      l'espace d'adressage. L'empoisonnement existant est le garde-fou.
//   2. _BitScanForward64 / __rdtsc. Substitutions directes.
//   3. GetWriteWatch. AUCUN equivalent : ni mprotect, ni gestionnaire de faute
//      de page, ni bits sales (la proposition memory-control n'est pas
//      expediee). C'est le seul verrou dur du portage, et il est traite par une
//      BARRIERE D'ECRITURE logicielle — voir `barrier` plus bas.
#include <atomic>
#include <emscripten.h>
#include <cstdlib>
#define R2V_WASM 1
// Il n'existe pas de compteur de cycles en wasm. `emscripten_get_now()` rend
// des millisecondes ; on compte donc en NANOSECONDES, ce qui fait tomber
// TscGhz() sur 1,0 et laisse toute la table de profil juste, sans la toucher.
//
// ATTENTION : c'est un appel vers JS. Sous node il est nanoseconde et bon
// marche ; dans le navigateur, `performance.now()` est BRIDE A 5 us en contexte
// cross-origin isolé — donc en isolation, la table --profile ne veut plus rien
// dire. Les juges du navigateur sont le temps de mur et les compteurs exacts.
static inline unsigned long long r2v_tsc_ns() {
	return static_cast<unsigned long long>(emscripten_get_now() * 1e6);
}
#define __rdtsc() r2v_tsc_ns()
#else
#include <intrin.h>   // _BitScanForward64
#include <windows.h>
#endif

// Point d'accroche declare par le patch lua/luaconf-customize.h. C'est par la
// que tout le heap Lua bascule dans l'arene.
#include "luaconf-customize.h"

#if defined(R2V_WASM)
// --- BARRIERE D'ECRITURE ---------------------------------------------------
//
// Remplace le suivi materiel des pages sales. Deux replis plus simples ont ete
// REFUTES avant d'ecrire ceci, et par la BANDE PASSANTE, pas par le CPU
// (docs/etude-portage-navigateur.md §4) :
//   - copie pleine de la zone servie : x21,4 sur les octets, 4,63 To en 26 s ;
//   - detection par comparaison au miroir : 10,8 To lus, meme mur.
// Ici le trafic reste celui du natif ; on ne paie qu'en instructions.
//
// Le bitmap couvre TOUTE la memoire lineaire et il est indexe par le numero de
// page ABSOLU. Consequence : la barriere n'a NI test d'intervalle NI
// branchement — un store vers la pile C marque un bit que personne ne lit,
// puisque SyncDirty ne balaye que la tranche de l'arene. C'est ce qui la rend
// assez bon marche pour etre posee devant chaque ecriture de ocgcore et de Lua.
//
// L'instrumentation vient de clang (-fsanitize-coverage=trace-stores), appliquee
// AUX SEULES unites de ocgcore et de Lua. Les intrinseques memoire (memcpy,
// memset, memmove) ne sont PAS instrumentees par ce passage : elles sont
// interceptees a l'edition de liens (-Wl,--wrap=) et marquees a la main. Le
// juge de correction n'est pas un raisonnement, c'est le harnais deja present :
// « test de fidelite de la restauration » et « test de stress des instantanes ».
namespace barrier {

constexpr size_t kPageShift = 12;
constexpr size_t kPageSize = size_t(1) << kPageShift;
// Couverture : TOUT l'espace adressable de wasm32 (4 Gio). A 4 Ko par page et
// 1 bit par page, cela fait 2^20 pages, soit 128 Ko de bitmap par thread —
// alloue une fois, jamais realloue, resident en cache. Couvrir tout l'espace
// est ce qui permet a la barriere de n'avoir ni test d'intervalle ni
// branchement : un store hors arene marque un bit que personne ne lit.
//
// PIEGE 32 BITS : ecrire `size_t(4) << 30` donne ZERO ici — size_t fait 32 bits
// en wasm32. Le bitmap devenait un tableau vide et chaque marquage ecrivait
// hors bornes. On compte donc en PAGES, jamais en octets.
constexpr size_t kPages = size_t(1) << (32 - kPageShift);
constexpr size_t kWords = kPages >> 6;

// PAS de `thread_local uint64_t bits[kWords]` : 128 Ko de TLS par thread font
// tomber le build threade d'emscripten (mesure : -fsanitize-coverage seul, ou
// -mno-bulk-memory-opt seul, suffisent alors a planter — l'image TLS est copiee
// par des instructions bulk a la naissance de chaque thread). Le TLS ne porte
// donc qu'un POINTEUR ; le bitmap vit sur le tas, un par thread, alloue par
// Arena::Init sur le thread proprietaire.
//
// `dummy` sert de cible aux threads qui n'ont pas d'arene : le marquage y est
// sans effet et personne ne le lit, ce qui evite un test de nullite dans la
// barriere — l'endroit ou une branche couterait le plus cher.
alignas(64) uint64_t dummy[kWords];
thread_local uint64_t* bits = dummy;

#define R2V_NOCOV __attribute__((no_sanitize("coverage")))

R2V_NOCOV inline void MarkPage(uintptr_t a) {
	const size_t page = a >> kPageShift;
	bits[page >> 6] |= uint64_t(1) << (page & 63);
}

// Dote le thread courant de son propre bitmap. Idempotent.
void EnsureBits() {
	if(bits != dummy)
		return;
	if(void* p = std::calloc(kWords, sizeof(uint64_t)))
		bits = static_cast<uint64_t*>(p);
}

// Pour les ecritures en bloc, que l'instrumentation ne voit pas.
R2V_NOCOV void MarkRange(const void* p, size_t n) {
	if(!n)
		return;
	uintptr_t a = reinterpret_cast<uintptr_t>(p);
	const uintptr_t end = a + n - 1;
	for(a &= ~(kPageSize - 1); a <= end; a += kPageSize)
		MarkPage(a);
}

// --- REGISTRE DES ARENES (variables GLOBALES, surtout PAS thread_local) -----
//
// Les crochets memcpy/memset sont poses sur TOUT le programme, y compris sur
// `__wasm_init_tls`, qui copie le bloc TLS d'un thread naissant... avec memcpy.
// Or `bits` VIT dans ce bloc TLS. Marquer depuis le crochet a ce moment-la
// ecrit a travers un TLS pas encore en place — et c'est exactement ce qui
// faisait tomber la recherche (le rejeu, lui, ne cree aucun thread : le bug
// etait invisible au test de fidelite).
//
// Le crochet ne touche donc au TLS QUE si la destination tombe dans une arene.
// Filtre en deux temps : l'enveloppe de toutes les arenes vivantes (deux
// comparaisons, jamais de TLS), puis, sur touche seulement, le registre exact —
// une adresse peut etre dans l'enveloppe sans etre dans aucune arene, et un
// bloc TLS pourrait justement s'y trouver.
constexpr size_t kMaxArenas = 64;
std::atomic<uintptr_t> hull_lo{ ~uintptr_t(0) }, hull_hi{ 0 };
std::atomic<uintptr_t> reg_lo[kMaxArenas], reg_hi[kMaxArenas];
std::atomic<size_t> reg_count{ 0 };

void Register(uintptr_t lo, uintptr_t hi) {
	const size_t i = reg_count.fetch_add(1, std::memory_order_relaxed);
	if(i < kMaxArenas) {
		reg_lo[i].store(lo, std::memory_order_relaxed);
		reg_hi[i].store(hi, std::memory_order_relaxed);
	}
	// L'enveloppe ne fait que grandir : un rétrecissement serait une course.
	uintptr_t cur = hull_lo.load(std::memory_order_relaxed);
	while(lo < cur && !hull_lo.compare_exchange_weak(cur, lo, std::memory_order_relaxed)) {}
	cur = hull_hi.load(std::memory_order_relaxed);
	while(hi > cur && !hull_hi.compare_exchange_weak(cur, hi, std::memory_order_relaxed)) {}
}

R2V_NOCOV inline bool InAnyArena(const void* p) {
	const uintptr_t a = reinterpret_cast<uintptr_t>(p);
	if(a < hull_lo.load(std::memory_order_relaxed) ||
	   a >= hull_hi.load(std::memory_order_relaxed))
		return false;
	const size_t n = reg_count.load(std::memory_order_relaxed);
	for(size_t i = 0; i < n && i < kMaxArenas; ++i)
		if(a >= reg_lo[i].load(std::memory_order_relaxed) &&
		   a < reg_hi[i].load(std::memory_order_relaxed))
			return true;
	return false;
}

// Analogue de ResetWriteWatch(base, committed) : efface la tranche du bitmap
// qui couvre l'arene. Appele apres une restauration, sinon les ecritures de la
// restauration ELLE-MEME s'accumuleraient (l'arene est alors identique au
// miroir : rien n'y est sale).
R2V_NOCOV void ClearSlice(uintptr_t base_addr, size_t bytes) {
	const size_t first = (base_addr >> kPageShift) >> 6;
	const size_t words = (bytes >> kPageShift) / 64 + 2;
	for(size_t w = 0; w < words; ++w)
		bits[first + w] = 0;
}

} // namespace barrier

#if !defined(R2V_NO_BARRIER)
extern "C" {
// Rappels de -fsanitize-coverage=trace-stores. Definis ici pour que le LTO
// puisse les replier dans le code instrumente : sans inlining, l'appel coute
// plus cher que le marquage lui-meme.
R2V_NOCOV void __sanitizer_cov_store1(void* p) { barrier::MarkPage(reinterpret_cast<uintptr_t>(p)); }
R2V_NOCOV void __sanitizer_cov_store2(void* p) { barrier::MarkPage(reinterpret_cast<uintptr_t>(p)); }
R2V_NOCOV void __sanitizer_cov_store4(void* p) { barrier::MarkPage(reinterpret_cast<uintptr_t>(p)); }
R2V_NOCOV void __sanitizer_cov_store8(void* p) { barrier::MarkPage(reinterpret_cast<uintptr_t>(p)); }
R2V_NOCOV void __sanitizer_cov_store16(void* p) { barrier::MarkPage(reinterpret_cast<uintptr_t>(p)); }
// Une ecriture de 16 octets peut chevaucher deux pages ; les autres tailles
// sont alignees par le compilateur et ne chevauchent jamais.
R2V_NOCOV void __sanitizer_cov_storeN(void* p, size_t n) { barrier::MarkRange(p, n); }

// Interception des ecritures en bloc (voir --wrap dans build_wasm.ps1).
void* __real_memcpy(void* d, const void* s, size_t n);
void* __real_memmove(void* d, const void* s, size_t n);
void* __real_memset(void* d, int c, size_t n);
R2V_NOCOV void* __wrap_memcpy(void* d, const void* s, size_t n) {
#if !defined(R2V_WRAP_INERT)
	if(barrier::InAnyArena(d))
		barrier::MarkRange(d, n);
#endif
	return __real_memcpy(d, s, n);
}
R2V_NOCOV void* __wrap_memmove(void* d, const void* s, size_t n) {
#if !defined(R2V_WRAP_INERT)
	if(barrier::InAnyArena(d))
		barrier::MarkRange(d, n);
#endif
	return __real_memmove(d, s, n);
}
R2V_NOCOV void* __wrap_memset(void* d, int c, size_t n) {
#if !defined(R2V_WRAP_INERT)
	if(barrier::InAnyArena(d))
		barrier::MarkRange(d, n);
#endif
	return __real_memset(d, c, n);
}
// --- ABAISSEMENT DES OPERATIONS EN BLOC (wasm-opt) --------------------------
//
// Avec des threads, `bulk-memory` est obligatoire : clang abaisse alors les
// copies de STRUCTURE en instruction `memory.copy`, invisible a
// -fsanitize-coverage comme a --wrap. Le verificateur chiffre le trou : 2 pages
// par run, et ce sont des tableaux de pointeurs — de la vraie corruption.
//
// `wasm-opt --llvm-memory-copy-fill-lowering` remplace ces instructions par des
// APPELS a `__memory_copy` / `__memory_fill`. En les DEFINISSANT ici, on
// reprend la main sur la seule ecriture qui echappait encore.
R2V_NOCOV void __memory_copy(void* d, const void* s, size_t n) {
	if(barrier::InAnyArena(d))
		barrier::MarkRange(d, n);
	__real_memmove(d, s, n);   // memmove : la source peut chevaucher
}
R2V_NOCOV void __memory_fill(void* d, int v, size_t n) {
	if(barrier::InAnyArena(d))
		barrier::MarkRange(d, n);
	__real_memset(d, v, n);
}
} // extern "C"
#endif   // !R2V_NO_BARRIER
#endif   // R2V_WASM

namespace solver {
namespace {

// Un span est l'unite de decoupe : 64 Ko decoupes en blocs d'une seule classe
// de taille. La classe se deduit de l'adresse, ce qui evite un en-tete par
// allocation — 16 octets d'en-tete doubleraient le cout des petits objets Lua,
// qui font typiquement 24 a 64 octets.
constexpr size_t kSpanShift = 16;
constexpr size_t kSpanSize = size_t(1) << kSpanShift;
constexpr size_t kAlign = 16;
constexpr size_t kMaxSmall = 32768;
constexpr uint8_t kFreeSpan = 0xff, kLargeHead = 0xfe, kLargeTail = 0xfd;
constexpr size_t kCommitChunk = size_t(1) << 20;

const size_t kClassSizes[] = {
	16,    32,    48,    64,    80,    96,    112,   128,
	160,   192,   224,   256,   320,   384,   448,   512,
	640,   768,   896,   1024,  1280,  1536,  1792,  2048,
	2560,  3072,  3584,  4096,  5120,  6144,  7168,  8192,
	10240, 12288, 14336, 16384, 20480, 24576, 28672, 32768,
};
constexpr size_t kNumClasses = sizeof(kClassSizes) / sizeof(kClassSizes[0]);
static_assert(kNumClasses < kLargeTail, "les classes doivent rester sous les marqueurs");

struct ClassTable {
	uint8_t lookup[kMaxSmall / kAlign + 1];
	ClassTable() {
		size_t k = 0;
		for(size_t i = 0; i <= kMaxSmall / kAlign; ++i) {
			size_t want = i * kAlign;
			while(k < kNumClasses && kClassSizes[k] < want)
				++k;
			lookup[i] = static_cast<uint8_t>(k < kNumClasses ? k : kNumClasses - 1);
		}
	}
};
const ClassTable& Classes() {
	static const ClassTable t;
	return t;
}

// Deux pointeurs distincts : `active` suit les scopes et decide OU allouer ;
// `owner` reste valable tant que l'arene existe et route les liberations.
thread_local Arena* t_active = nullptr;
thread_local Arena* t_owner = nullptr;
thread_local size_t t_host_fallbacks = 0;

} // namespace

Arena* CurrentArena() { return t_active; }
Arena* OwnerArena() { return t_owner; }

namespace detail {
void NoteHostFallback() { ++t_host_fallbacks; }
} // namespace detail

ArenaScope::ArenaScope(Arena* a) : previous(t_active) { t_active = a; }
ArenaScope::~ArenaScope() { t_active = previous; }

ArenaPause::ArenaPause() : previous(t_active) { t_active = nullptr; }
ArenaPause::~ArenaPause() { t_active = previous; }

bool Arena::Init(size_t reserve, std::uintptr_t preferred_base, std::string& error) {
#if defined(R2V_WASM)
	(void)preferred_base;   // une seule memoire lineaire : la base est ce qu'elle est
	page_size = barrier::kPageSize;
	reserve = (reserve + kSpanSize - 1) & ~(kSpanSize - 1);
	// Alignement sur 64 pages : la tranche du bitmap de la barriere tombe alors
	// sur une frontiere de MOT, et SyncDirty la verse sans decalage.
	constexpr size_t kBaseAlign = barrier::kPageSize * 64;
	base = static_cast<uint8_t*>(std::aligned_alloc(kBaseAlign,
					(reserve + kBaseAlign - 1) & ~(kBaseAlign - 1)));
	write_watch = base != nullptr;   // la barriere joue le role du suivi materiel
	if(!base) {
		error = "aligned_alloc a echoue pour " + std::to_string(reserve) + " octets";
		return false;
	}
	barrier::EnsureBits();
	// Fait connaitre la plage aux crochets memoire : sans cela ils ne peuvent
	// pas savoir, SANS toucher au TLS, si une copie en bloc vise l'arene.
	barrier::Register(reinterpret_cast<uintptr_t>(base),
					  reinterpret_cast<uintptr_t>(base) + reserve);
#else
	SYSTEM_INFO si{};
	GetSystemInfo(&si);
	page_size = si.dwPageSize;
	reserve = (reserve + kSpanSize - 1) & ~(kSpanSize - 1);

	// MEM_WRITE_WATCH ne sert pas encore a restaurer, mais il permet de compter
	// les pages qu'une action salit — le chiffre dont depend le passage a une
	// restauration incrementale.
	auto reserve_at = [&](LPVOID at, bool watch) -> uint8_t* {
		DWORD flags = MEM_RESERVE | (watch ? MEM_WRITE_WATCH : 0u);
		return static_cast<uint8_t*>(VirtualAlloc(at, reserve, flags, PAGE_READWRITE));
	};

	base = reserve_at(reinterpret_cast<LPVOID>(preferred_base), true);
	write_watch = base != nullptr;
	if(!base) {
		// L'adresse demandee peut etre prise. A l'interieur d'un processus la
		// base fixe n'est pas necessaire : elle ne le deviendra que pour porter
		// un instantane d'un processus a l'autre.
		base = reserve_at(nullptr, true);
		write_watch = base != nullptr;
	}
	if(!base) {
		base = reserve_at(nullptr, false);
		write_watch = false;
	}
	if(!base) {
		error = "VirtualAlloc a echoue pour " + std::to_string(reserve) + " octets";
		return false;
	}
#endif

	base_addr = reinterpret_cast<std::uintptr_t>(base);
	reserved = reserve;
	committed = 0;
	next_span = 0;
	spans.assign(reserve / kSpanSize, SpanInfo{});
	free_lists.assign(kNumClasses, nullptr);
	free_span_runs.clear();
	live_bytes = alloc_count = free_count = 0;

	// PIEGE : ces conteneurs appartiennent a l'allocateur lui-meme. Les laisser
	// grandir pendant que l'arene est active les ferait allouer DANS l'arene
	// qu'ils administrent — et une restauration ecraserait alors leur contenu
	// sous leurs propres pieds. On les dimensionne ici, une fois pour toutes,
	// alors qu'aucune arene n'est encore active : plus aucune reallocation
	// ensuite. Meme raison pour le tampon de pages sales.
	free_span_runs.reserve(spans.size() + 1);
	dirty_scratch.reserve(reserve / page_size + 1);

	t_owner = this;   // apres les reservations, sinon elles seraient reroutees
	return true;
}

void Arena::Shutdown() {
	// Les conteneurs ci-dessous vivent hors arene, mais on coupe le routage
	// avant de les detruire pour que plus aucune liberation ne soit reroutee.
	if(t_active == this)
		t_active = nullptr;
	if(t_owner == this)
		t_owner = nullptr;
	checkpoints.clear();
	spans.clear();
	free_lists.clear();
	free_span_runs.clear();
	dirty_scratch.clear();
	if(base) {
#if defined(R2V_WASM)
		std::free(base);
#else
		VirtualFree(base, 0, MEM_RELEASE);
#endif
		base = nullptr;
	}
	reserved = committed = 0;
}

bool Arena::CommitTo(size_t offset) {
	if(offset <= committed)
		return true;
	size_t want = (offset + kCommitChunk - 1) & ~(kCommitChunk - 1);
	want = (std::min)(want, reserved);
	if(want <= committed)
		return false;
#if defined(R2V_WASM)
	// Rien a demander au systeme : la plage est deja a nous. `committed` reste
	// un FILIGRANE — il borne le miroir et le balayage de SyncDirty, donc il
	// garde tout son sens. La page est mise a zero pour que le miroir parte
	// d'un contenu defini (VirtualAlloc(MEM_COMMIT) le faisait gratuitement).
	std::memset(base + committed, 0, want - committed);
#else
	if(!VirtualAlloc(base + committed, want - committed, MEM_COMMIT, PAGE_READWRITE))
		return false;
#endif
	committed = want;
	return true;
}

void* Arena::AllocSpans(size_t span_count) {
	for(size_t i = 0; i < free_span_runs.size(); ++i) {
		size_t idx = free_span_runs[i];
		if(spans[idx].run >= span_count) {
			size_t have = spans[idx].run;
			free_span_runs.erase(free_span_runs.begin() + i);
			if(have > span_count) {
				size_t rest = idx + span_count;
				spans[rest].klass = kFreeSpan;
				spans[rest].run = static_cast<uint32_t>(have - span_count);
				free_span_runs.push_back(rest);
			}
			return base + idx * kSpanSize;
		}
	}
	if((next_span + span_count) * kSpanSize > reserved)
		return nullptr;
	size_t idx = next_span;
	if(!CommitTo((idx + span_count) * kSpanSize))
		return nullptr;
	next_span += span_count;
	return base + idx * kSpanSize;
}

void Arena::FreeSpans(size_t span_index, size_t span_count) {
	for(size_t i = 0; i < span_count; ++i)
		spans[span_index + i] = SpanInfo{};
	spans[span_index].klass = kFreeSpan;
	spans[span_index].run = static_cast<uint32_t>(span_count);
	free_span_runs.push_back(span_index);
}

// FERMETURE DU TROU `memory.copy` (wasm threade).
//
// Avec des threads, `bulk-memory` est obligatoire — donc clang abaisse les
// copies de STRUCTURE en instruction `memory.copy`, que ni
// -fsanitize-coverage ni --wrap ne voient. Constat du verificateur : 2 pages
// echappaient a chaque run.
//
// Or ces copies visent presque toujours un bloc QUI VIENT D'ETRE ALLOUE. On
// marque donc le bloc a l'allocation : n'importe quelle ecriture ulterieure,
// vue ou non par l'instrumentation, tombe alors sur une page deja sale. Le
// sur-marquage est toujours SUR (il recopie une page de trop, jamais une de
// moins) et il coute une poignee d'instructions par allocation.
//
// Ce qui reste dehors — une copie en bloc vers un objet DEJA VIEUX — est ce que
// le verificateur doit continuer de chiffrer a zero.
void Arena::MarkFresh(const void* p, size_t n) {
#if defined(R2V_WASM)
	barrier::MarkRange(p, n);
#else
	(void)p; (void)n;
#endif
}

void* Arena::Allocate(size_t size) {
	if(size == 0)
		size = 1;
	++alloc_count;
	// Compte seulement, jamais d'horloge : ~10^6 appels/s par worker, un rdtsc
	// par appel fabriquerait le ralentissement qu'il pretend observer.
	prof::Count(prof::kAlloc);

	if(size > kMaxSmall) {
		size_t span_count = (size + kSpanSize - 1) >> kSpanShift;
		void* p = AllocSpans(span_count);
		if(!p)
			return nullptr;
		size_t idx = (static_cast<uint8_t*>(p) - base) >> kSpanShift;
		spans[idx].klass = kLargeHead;
		spans[idx].run = static_cast<uint32_t>(span_count);
		for(size_t i = 1; i < span_count; ++i)
			spans[idx + i].klass = kLargeTail;
		live_bytes += span_count * kSpanSize;
		MarkFresh(p, span_count * kSpanSize);
		return p;
	}

	size_t klass = Classes().lookup[(size + kAlign - 1) / kAlign];
	void*& head = free_lists[klass];
	if(!head) {
		// Plus de bloc libre : on prend un span et on le decoupe entierement.
		void* span = AllocSpans(1);
		if(!span)
			return nullptr;
		size_t idx = (static_cast<uint8_t*>(span) - base) >> kSpanShift;
		spans[idx].klass = static_cast<uint8_t>(klass);
		spans[idx].run = 1;
		size_t bs = kClassSizes[klass];
		auto* cur = static_cast<uint8_t*>(span);
		for(size_t off = 0; off + bs <= kSpanSize; off += bs) {
			*reinterpret_cast<void**>(cur + off) = head;
			head = cur + off;
		}
	}
	void* p = head;
	head = *reinterpret_cast<void**>(p);
	live_bytes += kClassSizes[klass];
	MarkFresh(p, kClassSizes[klass]);
	return p;
}

void Arena::Free(void* ptr) {
	if(!ptr || !Contains(ptr))
		return;
	++free_count;
	prof::Count(prof::kFree);
	size_t idx = (static_cast<uint8_t*>(ptr) - base) >> kSpanShift;
	uint8_t klass = spans[idx].klass;
	if(klass == kLargeHead) {
		size_t run = spans[idx].run;
		live_bytes -= (std::min)(live_bytes, run * kSpanSize);
		FreeSpans(idx, run);
		return;
	}
	if(klass >= kNumClasses)
		return; // span libre ou suite de bloc large : pointeur non alloue ici
	*reinterpret_cast<void**>(ptr) = free_lists[klass];
	free_lists[klass] = ptr;
	live_bytes -= (std::min)(live_bytes, kClassSizes[klass]);
}

void* Arena::Reallocate(void* ptr, size_t old_size, size_t new_size) {
	prof::Count(prof::kRealloc);
	if(!ptr)
		return Allocate(new_size);
	if(new_size == 0) {
		Free(ptr);
		return nullptr;
	}
	// Si la nouvelle taille tient dans la meme classe, on ne bouge rien : Lua
	// redimensionne ses tampons par petits increments, ce cas est frequent.
	if(Contains(ptr) && new_size <= kMaxSmall) {
		size_t idx = (static_cast<uint8_t*>(ptr) - base) >> kSpanShift;
		uint8_t klass = spans[idx].klass;
		if(klass < kNumClasses && new_size <= kClassSizes[klass])
			return ptr;
	}
	void* np = Allocate(new_size);
	if(!np)
		return nullptr;
	std::memcpy(np, ptr, (std::min)(old_size, new_size));
	Free(ptr);
	return np;
}

void* Arena::LuaAlloc(void* ud, void* ptr, size_t osize, size_t nsize) {
	auto* self = static_cast<Arena*>(ud);
	if(nsize == 0) {
		self->Free(ptr);
		return nullptr;
	}
	if(!ptr)
		return self->Allocate(nsize);
	return self->Reallocate(ptr, osize, nsize);
}

void Arena::EnsureMirror() {
	if(mirror.size() >= committed)
		return;
	size_t old = mirror.size();
	mirror.resize(committed);
	// Les pages neuves n'ont jamais ete photographiees : on les aligne sur
	// l'arene, sinon une restauration y ecrirait n'importe quoi.
	std::memcpy(mirror.data() + old, base + old, committed - old);
}

size_t Arena::SyncDirty() {
	if(!write_watch || !committed)
		return 0;
#if defined(R2V_WASM) && defined(R2V_NO_BARRIER)
	// TEMOIN. Sans barriere, on ne sait rien : tout ce qui est servi est
	// declare sale. C'est CORRECT (le repli deja prevu quand GetWriteWatch
	// echoue) et c'est le bras contre lequel se mesure la barriere.
	const size_t pages = committed / page_size + 1;
	const size_t words = (pages + 63) / 64 + 1;
	if(!checkpoints.empty()) {
		std::vector<uint64_t>& bits = checkpoints.back().dirty;
		if(bits.size() < words)
			bits.resize(words, 0);
		std::fill(bits.begin(), bits.end(), ~uint64_t(0));
	}
	return pages;
#elif defined(R2V_WASM)
	// R2V_ARENA_ALLDIRTY=1 : MEME binaire, dirty-set complet. C'est le seul
	// moyen de departager « la barriere est incomplete » de « autre chose est
	// casse » sans changer une seule ligne de code genere.
	static const bool all_dirty = [] {
		const char* e = std::getenv("R2V_ARENA_ALLDIRTY");
		return e && *e && *e != '0';
	}();
	if(all_dirty) {
		const size_t pages = committed / page_size + 1;
		const size_t nw = (pages + 63) / 64 + 1;
		barrier::ClearSlice(base_addr, committed);
		if(!checkpoints.empty()) {
			std::vector<uint64_t>& b = checkpoints.back().dirty;
			if(b.size() < nw)
				b.resize(nw, 0);
			std::fill(b.begin(), b.end(), ~uint64_t(0));
		}
		return pages;
	}
	// Verse la tranche du bitmap de la barriere qui couvre l'arene dans le
	// niveau courant, ET la remet a zero — meme semantique que
	// GetWriteWatch(WRITE_WATCH_FLAG_RESET). La base etant alignee sur 64
	// pages, la tranche commence sur une frontiere de mot : pas de decalage.
	const size_t first = (base_addr >> barrier::kPageShift) >> 6;
	const size_t words = (committed / page_size + 63) / 64 + 1;
	std::vector<uint64_t>* bits = nullptr;
	if(!checkpoints.empty()) {
		bits = &checkpoints.back().dirty;
		if(bits->size() < words)
			bits->resize(words, 0);
	}
	size_t count = 0;
	for(size_t w = 0; w < words; ++w) {
		const uint64_t v = barrier::bits[first + w];
		if(!v)
			continue;
		barrier::bits[first + w] = 0;
		count += __builtin_popcountll(v);
		if(bits)
			(*bits)[w] |= v;
	}
	return count;
#else
	size_t capacity = committed / page_size + 1;
	if(capacity > dirty_scratch.capacity())
		return 0;
	dirty_scratch.resize(capacity);
	ULONG_PTR count = capacity;
	ULONG granularity = 0;
	size_t words = capacity / 64 + 2;
	std::vector<uint64_t>* bits = nullptr;
	if(!checkpoints.empty()) {
		bits = &checkpoints.back().dirty;
		if(bits->size() < words)
			bits->resize(words, 0);
	}
	if(GetWriteWatch(WRITE_WATCH_FLAG_RESET, base, committed,
					 reinterpret_cast<PVOID*>(dirty_scratch.data()),
					 &count, &granularity) != 0) {
		// En cas d'echec on considere tout comme sale : correct, juste lent.
		if(bits)
			std::fill(bits->begin(), bits->end(), ~uint64_t(0));
		return capacity;
	}
	if(bits) {
		for(ULONG_PTR i = 0; i < count; ++i) {
			size_t page = size_t(dirty_scratch[i] - base) / page_size;
			(*bits)[page >> 6] |= uint64_t(1) << (page & 63);
		}
	}
	return static_cast<size_t>(count);
#endif
}

#if defined(R2V_WASM)
namespace {
std::atomic<uint64_t> g_verify_checked{ 0 }, g_verify_missed{ 0 };
}

bool Arena::VerifyBarrier() {
	static const bool on = [] {
		const char* e = std::getenv("R2V_ARENA_VERIFY");
		return e && *e && *e != '0';
	}();
	return on;
}

void Arena::VerifyDirtySet(const Checkpoint& cp) {
	const size_t limit = (std::min)(cp.in_use, mirror.size());
	uint64_t missed = 0;
	for(size_t off = 0; off + page_size <= limit; off += page_size) {
		if(std::memcmp(base + off, mirror.data() + off, page_size) == 0)
			continue;
		const size_t page = off / page_size;
		const bool marked = (page >> 6) < cp.dirty.size() &&
							(cp.dirty[page >> 6] & (uint64_t(1) << (page & 63)));
		if(!marked) {
			if(missed == 0 && g_verify_missed.load(std::memory_order_relaxed) < 4) {
				std::fprintf(stderr,
							 "!! barriere INCOMPLETE : page %zu (offset %zu) differe "
							 "du miroir sans etre marquee\n", page, off);
				// QUOI a change, et pas seulement OU : du texte designe une
				// fonction de libc non interceptee, des pointeurs une copie de
				// structure abaissee en `memory.copy`.
				size_t shown = 0;
				for(size_t k = 0; k + 16 <= page_size && shown < 3; ++k) {
					if(base[off + k] == mirror[off + k])
						continue;
					++shown;
					std::fprintf(stderr, "   +%04zu miroir:", k);
					for(size_t j = 0; j < 16; ++j)
						std::fprintf(stderr, " %02x", mirror[off + k + j]);
					std::fprintf(stderr, "\n         arene :");
					for(size_t j = 0; j < 16; ++j)
						std::fprintf(stderr, " %02x", base[off + k + j]);
					std::fprintf(stderr, "\n         texte : ");
					for(size_t j = 0; j < 16; ++j) {
						const uint8_t c = base[off + k + j];
						std::fprintf(stderr, "%c", (c >= 32 && c < 127) ? c : '.');
					}
					std::fprintf(stderr, "\n");
					k += 15;
				}
			}
			++missed;
		}
	}
	g_verify_checked.fetch_add(limit / page_size, std::memory_order_relaxed);
	if(missed)
		g_verify_missed.fetch_add(missed, std::memory_order_relaxed);
}

void Arena::PrintVerifyReport() {
	if(!VerifyBarrier())
		return;
	const uint64_t checked = g_verify_checked.load(std::memory_order_relaxed);
	const uint64_t missed = g_verify_missed.load(std::memory_order_relaxed);
	std::printf("\n--- verificateur de la barriere d'ecriture ---\n"
				"  pages comparees au miroir : %llu\n"
				"  pages sales NON marquees  : %llu  %s\n",
				(unsigned long long)checked, (unsigned long long)missed,
				missed ? "<-- CORRUPTION : la barriere laisse passer des ecritures"
					   : "(aucune : la barriere capture tout ce qui a bouge)");
}
#endif

void Arena::CaptureMetadata(Checkpoint& cp) const {
	cp.in_use = next_span * kSpanSize;
	cp.spans.assign(spans.begin(), spans.begin() + next_span);
	cp.free_lists = free_lists;
	cp.free_span_runs = free_span_runs;
	cp.next_span = next_span;
	cp.live_bytes = live_bytes;
}

void Arena::RestoreMetadata(const Checkpoint& cp) {
	std::copy(cp.spans.begin(), cp.spans.end(), spans.begin());
	// Les spans nes apres le point de reprise redeviennent vierges.
	for(size_t i = cp.next_span; i < next_span; ++i)
		spans[i] = SpanInfo{};
	free_lists = cp.free_lists;
	free_span_runs = cp.free_span_runs;
	next_span = cp.next_span;
	live_bytes = cp.live_bytes;
}

// Parcourt les pages marquees d'un bitmap.
template<typename F>
static void ForEachDirtyPage(const std::vector<uint64_t>& bits, F&& fn) {
	for(size_t w = 0; w < bits.size(); ++w) {
		uint64_t word = bits[w];
		while(word) {
#if defined(R2V_WASM)
			const unsigned b = __builtin_ctzll(word);
#else
			unsigned long b;
			_BitScanForward64(&b, word);
#endif
			word &= word - 1;
			fn((w << 6) + b);
		}
	}
}

void Arena::Push() {
	prof::Scope ps(prof::kArenaPush);
	// Les tampons de l'instantane sont a l'hote : sans cette pause ils seraient
	// alloues dans l'arene qu'ils sont censes photographier.
	ArenaPause off;
	EnsureMirror();
	SyncDirty();

	size_t pages = 0;
	if(!checkpoints.empty()) {
		// Le niveau courant devient parent : on lui constitue le journal qui
		// permettra de ramener le miroir en arriere quand le fils sera depile.
		Checkpoint& parent = checkpoints.back();
		parent.undo_pages.clear();
		parent.undo_data.clear();
		ForEachDirtyPage(parent.dirty, [&](size_t page) {
			size_t off_p = page * page_size;
			if(off_p + page_size > mirror.size())
				return;
			// Deux operations aux bornes DIFFERENTES, ne pas les confondre.
			//
			// L'image d'avant-modification n'a de sens que pour les pages qui
			// existaient deja a l'empilement du parent : au-dela, le span
			// n'etait pas alloue et son contenu n'a pas a etre retabli.
			if(off_p < parent.in_use) {
				parent.undo_pages.push_back(uint32_t(page));
				size_t at = parent.undo_data.size();
				parent.undo_data.resize(at + page_size);
				std::memcpy(parent.undo_data.data() + at, mirror.data() + off_p,
							page_size);
			}
			// Le miroir, lui, doit refleter l'etat du fils SANS exception :
			// c'est de la qu'une restauration du fils tirera ses pages. Une
			// page nee pendant la periode du parent y serait sinon lue perimee.
			std::memcpy(mirror.data() + off_p, base + off_p, page_size);
			++pages;
		});
	} else {
		// Premier niveau : le miroir doit refleter l'arene entiere.
		std::memcpy(mirror.data(), base, (std::min)(mirror.size(), committed));
		pages = committed / page_size;
	}

	Checkpoint cp;
	CaptureMetadata(cp);
	cp.dirty.assign(committed / page_size / 64 + 2, 0);
	checkpoints.push_back(std::move(cp));
	last_push = { pages, pages * page_size };
	prof::Count(prof::kPagesPushed, pages);
}

void Arena::Restore() {
	if(checkpoints.empty())
		return;
	prof::Scope ps(prof::kArenaRestore);
	ArenaPause off;
	SyncDirty();
	Checkpoint& cp = checkpoints.back();
#if defined(R2V_WASM)
	// VERIFICATEUR DE LA BARRIERE (R2V_ARENA_VERIFY=1).
	//
	// La barriere logicielle n'est pas prouvable par lecture : clang peut
	// abaisser une copie de structure en `memory.copy`, que ni
	// -fsanitize-coverage ni --wrap ne voient. On ne SUPPOSE donc pas qu'elle
	// est complete — on l'exige, et on la mesure : toute page qui differe du
	// miroir sans etre marquee est une page que Restore() ne remettrait pas en
	// place, c'est-a-dire une corruption silencieuse.
	//
	// Cout : un memcmp de la zone servie par restauration. Reserve au diagnostic.
	if(VerifyBarrier())
		VerifyDirtySet(cp);
#endif
	size_t pages = 0;
	ForEachDirtyPage(cp.dirty, [&](size_t page) {
		size_t off_p = page * page_size;
		if(off_p + page_size > mirror.size() || off_p >= cp.in_use)
			return;
		std::memcpy(base + off_p, mirror.data() + off_p, page_size);
		++pages;
	});
	RestoreMetadata(cp);
	// L'arene est de nouveau identique au miroir : plus rien n'a bouge depuis
	// l'empilement de ce niveau. On repart d'un suivi vierge, sinon les pages
	// ecrites par la restauration elle-meme s'accumuleraient.
	std::fill(cp.dirty.begin(), cp.dirty.end(), 0);
#if defined(R2V_WASM)
	// L'arene vient d'etre remise a l'etat du miroir. Les ecritures que la
	// restauration a faites ELLE-MEME ont marque des bits : il faut les jeter,
	// exactement comme ResetWriteWatch le fait cote Windows.
	barrier::ClearSlice(base_addr, committed);
#else
	if(write_watch && committed)
		ResetWriteWatch(base, committed);
#endif
	last_restore = { pages, pages * page_size };
	prof::Count(prof::kPagesRestored, pages);
}

void Arena::Pop() {
	if(checkpoints.empty())
		return;
	// Le Restore() interne s'impute a sa propre sonde ; le self d'arene Pop est
	// la fusion des pages sales et le retablissement du miroir.
	prof::Scope ps(prof::kArenaPop);
	// Etat du fils au moment de son empilement, puis retrait du niveau.
	std::vector<uint64_t> child_dirty;
	{
		ArenaPause off;
		SyncDirty();
		child_dirty = checkpoints.back().dirty;
	}
	Restore();
	ArenaPause off;
	checkpoints.pop_back();
	if(checkpoints.empty())
		return;

	Checkpoint& parent = checkpoints.back();
	// Ce que le fils a modifie a aussi bouge depuis l'empilement du parent.
	if(parent.dirty.size() < child_dirty.size())
		parent.dirty.resize(child_dirty.size(), 0);
	for(size_t i = 0; i < child_dirty.size(); ++i)
		parent.dirty[i] |= child_dirty[i];

	// Le miroir doit revenir a l'etat du parent.
	for(size_t i = 0; i < parent.undo_pages.size(); ++i) {
		size_t off_p = size_t(parent.undo_pages[i]) * page_size;
		if(off_p + page_size <= mirror.size())
			std::memcpy(mirror.data() + off_p,
						parent.undo_data.data() + i * page_size, page_size);
	}
	parent.undo_pages.clear();
	parent.undo_data.clear();
}

void Arena::PopToAndRestore(size_t k) {
	if(k == 0) {
		Restore();
		return;
	}
	if(checkpoints.size() < k + 1)
		k = checkpoints.empty() ? 0 : checkpoints.size() - 1;
	if(k == 0) {
		Restore();
		return;
	}
	prof::Scope ps(prof::kArenaPop);
	ArenaPause off;
	SyncDirty();
	const size_t target = checkpoints.size() - 1 - k;
	// L'union des pages modifiees depuis l'empilement du niveau cible : sa
	// periode propre plus celles des k niveaux depiles. (La fusion montante
	// des Pop sequentiels construit exactement cet ensemble, un niveau a la
	// fois ; ici on le prend d'un coup.)
	std::vector<uint64_t> uni = checkpoints[target].dirty;
	for(size_t i = target + 1; i < checkpoints.size(); ++i) {
		const std::vector<uint64_t>& d = checkpoints[i].dirty;
		if(uni.size() < d.size())
			uni.resize(d.size(), 0);
		for(size_t w = 0; w < d.size(); ++w)
			uni[w] |= d[w];
	}
	// Ramener le MIROIR a l'etat de l'empilement du niveau cible : le journal
	// du niveau i ramene le miroir de l'empilement de son fils au sien —
	// application du haut vers le bas, chaque journal consomme.
	for(size_t i = checkpoints.size() - 1; i-- > target;) {
		Checkpoint& cp = checkpoints[i];
		for(size_t j = 0; j < cp.undo_pages.size(); ++j) {
			size_t off_p = size_t(cp.undo_pages[j]) * page_size;
			if(off_p + page_size <= mirror.size())
				std::memcpy(mirror.data() + off_p,
							cp.undo_data.data() + j * page_size, page_size);
		}
		cp.undo_pages.clear();
		cp.undo_data.clear();
	}
	// L'arene depuis le miroir, chaque page UNE fois. La borne est l'in_use du
	// niveau CIBLE : au-dela, les spans n'existent pas a ce niveau —
	// RestoreMetadata les rend vierges, et Allocate reconstruit leurs chaines
	// en les recarvant avant tout usage (le contenu residuel n'est jamais lu).
	Checkpoint& tcp = checkpoints[target];
	size_t pages = 0;
	ForEachDirtyPage(uni, [&](size_t page) {
		size_t off_p = page * page_size;
		if(off_p + page_size > mirror.size() || off_p >= tcp.in_use)
			return;
		std::memcpy(base + off_p, mirror.data() + off_p, page_size);
		++pages;
	});
	checkpoints.resize(target + 1);
	RestoreMetadata(tcp);
	std::fill(tcp.dirty.begin(), tcp.dirty.end(), 0);
#if defined(R2V_WASM)
	// L'arene vient d'etre remise a l'etat du miroir. Les ecritures que la
	// restauration a faites ELLE-MEME ont marque des bits : il faut les jeter,
	// exactement comme ResetWriteWatch le fait cote Windows.
	barrier::ClearSlice(base_addr, committed);
#else
	if(write_watch && committed)
		ResetWriteWatch(base, committed);
#endif
	last_restore = { pages, pages * page_size };
	prof::Count(prof::kPagesRestored, pages);
}

void Arena::Discard() {
	ArenaPause off;
	if(!checkpoints.empty())
		checkpoints.pop_back();
}

size_t Arena::UndoBytes() const {
	size_t n = mirror.capacity();
	for(const auto& cp : checkpoints)
		n += cp.undo_data.capacity() + cp.dirty.capacity() * 8 +
			 cp.spans.capacity() * sizeof(SpanInfo);
	return n;
}

void Arena::ResetDirtyTracking() {
	ArenaPause off;
	SyncDirty();
}

size_t Arena::CountDirtyPages() {
	// Passe imperativement par SyncDirty : lire le suivi materiel le remet a
	// zero, donc une lecture qui ne verserait pas les bits dans le niveau
	// courant les perdrait, et la restauration serait incomplete.
	ArenaPause off;
	return SyncDirty();
}

std::string Arena::SelfCheck() const {
	struct { const char* name; const void* p; size_t cap; } checks[] = {
		{ "spans", spans.data(), spans.capacity() },
		{ "free_lists", free_lists.data(), free_lists.capacity() },
		{ "free_span_runs", free_span_runs.data(), free_span_runs.capacity() },
		{ "dirty_scratch", dirty_scratch.data(), dirty_scratch.capacity() },
		{ "mirror", mirror.data(), mirror.capacity() },
	};
	for(const auto& c : checks) {
		if(c.p && Contains(c.p))
			return std::string("la structure interne '") + c.name +
				   "' vit dans l'arene : une restauration la corromprait";
	}
	if(free_span_runs.size() == free_span_runs.capacity() && !free_span_runs.empty())
		return "free_span_runs est plein : la prochaine insertion reallouerait";
	return {};
}

ArenaStats Arena::Stats() const {
	ArenaStats s;
	s.reserved = reserved;
	s.committed = committed;
	s.in_use = next_span * kSpanSize;
	s.live_bytes = live_bytes;
	s.alloc_count = alloc_count;
	s.free_count = free_count;
	// Compteur MEMBRE, pas thread_local : les replis se produisent dans les
	// workers et ce bilan se lit depuis le thread principal (C7).
	s.host_fallbacks = fallbacks.load(std::memory_order_relaxed);
	s.poisoned = poisoned.load(std::memory_order_relaxed);
	return s;
}

// --- profil du chemin chaud (--profile) --------------------------------------

namespace prof {

bool enabled = false;

namespace {

const char* const kSiteNames[kSiteCount] = {
	"recherche (reste)",     // kSearch : self du corps des Run*
	"rejeu prefixe (reste)", // kPrefix
	"Process (core)",        // kProcess
	"Query (zones)",         // kQuery
	"QueryCodes",            // kQueryCodes
	"ProcessorState",        // kProcState
	"QueryCount",            // kCount
	"enumeration",           // kEnumerate
	"digest (self)",         // kDigest
	"board key (self)",      // kBoardKey
	"atomes IW (self)",      // kAtoms
	"recettes (self)",       // kRecipe
	"arene Push",            // kArenaPush
	"arene Restore",         // kArenaRestore
	"arene Pop",             // kArenaPop
};

struct TlBuf;
void FlushBuf(TlBuf& b);

// Compteurs du thread, verses aux atomiques globaux par le destructeur (mort
// du thread — les workers sont joints par phase) ou par FlushThread (thread
// principal). Aucun atomique sur le chemin par-appel (piege 58 et regle 2 de
// l'etape 1 du chantier perf).
struct TlBuf {
	uint64_t calls[kSiteCount] = {};
	uint64_t ticks[kSiteCount] = {};   // temps EXCLUSIF (self)
	uint64_t counters[kCounterCount] = {};
	// Schema du temps exclusif : cumul des durees INCLUSIVES des scopes fermes.
	// Un scope englobant lit ce que ses enfants ont consomme pendant sa periode
	// (child - child0) et le soustrait de son propre temps ; a sa propre
	// fermeture il credite sa duree inclusive a son parent.
	uint64_t child = 0;
	~TlBuf() { FlushBuf(*this); }
};

std::atomic<uint64_t> g_calls[kSiteCount];
std::atomic<uint64_t> g_ticks[kSiteCount];
std::atomic<uint64_t> g_counters[kCounterCount];

// Cumul du run entier, alimente a chaque remise a zero de phase.
uint64_t g_run_calls[kSiteCount];
uint64_t g_run_ticks[kSiteCount];
uint64_t g_run_counters[kCounterCount];

uint64_t g_tsc0 = 0;
std::chrono::steady_clock::time_point g_t0;

TlBuf& Tl() {
	static thread_local TlBuf b;
	return b;
}

void FlushBuf(TlBuf& b) {
	for(uint32_t i = 0; i < kSiteCount; ++i) {
		if(b.calls[i])
			g_calls[i].fetch_add(b.calls[i], std::memory_order_relaxed);
		if(b.ticks[i])
			g_ticks[i].fetch_add(b.ticks[i], std::memory_order_relaxed);
		b.calls[i] = b.ticks[i] = 0;
	}
	for(uint32_t i = 0; i < kCounterCount; ++i) {
		if(b.counters[i])
			g_counters[i].fetch_add(b.counters[i], std::memory_order_relaxed);
		b.counters[i] = 0;
	}
}

// La frequence tsc se calibre contre l'horloge murale sur TOUTE la duree
// ecoulee depuis Enable() : gratuite, et d'autant plus precise que le run est
// long.
double TscGhz() {
	const double s = std::chrono::duration<double>(
						 std::chrono::steady_clock::now() - g_t0).count();
	if(s <= 0)
		return 3.0;   // valeur de survie, jamais atteinte en pratique
	return double(__rdtsc() - g_tsc0) / s / 1e9;
}

void PrintTable(const char* label, const uint64_t calls[], const uint64_t ticks[],
				const uint64_t counters[]) {
	const double ghz = TscGhz();
	uint64_t total = 0;
	for(uint32_t i = 0; i < kSiteCount; ++i)
		total += ticks[i];
	if(!total)
		return;
	const uint64_t dec = counters[kDecisions];
	std::printf("\n--- profil [%s] (tsc %.2f GHz) ---\n", label, ghz);
	std::printf("  %-22s %12s %9s %12s %9s %6s\n", "sonde", "appels", "/dec",
				"total", "us/appel", "part");
	// Tri par temps decroissant : le profil se lit de haut en bas.
	uint32_t order[kSiteCount];
	for(uint32_t i = 0; i < kSiteCount; ++i)
		order[i] = i;
	std::sort(order, order + kSiteCount, [&](uint32_t a, uint32_t b) {
		return ticks[a] > ticks[b];
	});
	for(uint32_t k = 0; k < kSiteCount; ++k) {
		const uint32_t i = order[k];
		if(!calls[i] && !ticks[i])
			continue;
		const double s = double(ticks[i]) / ghz / 1e9;
		std::printf("  %-22s %12llu %9.2f %10.3f s %9.2f %5.1f%%\n",
					kSiteNames[i], (unsigned long long)calls[i],
					dec ? double(calls[i]) / double(dec) : 0.0, s,
					calls[i] ? s * 1e6 / double(calls[i]) : 0.0,
					100.0 * double(ticks[i]) / double(total));
	}
	const double ts = double(total) / ghz / 1e9;
	std::printf("  %-22s %12s %9s %10.3f s\n", "total mesure", "", "", ts);
	if(dec)
		std::printf("  decisions : %llu  (%.1f us par decision, sondes comprises)\n",
					(unsigned long long)dec, ts * 1e6 / double(dec));
	if(counters[kAlloc] || counters[kFree])
		std::printf("  arene : alloc %llu (%.1f/dec)  free %llu  realloc %llu\n",
					(unsigned long long)counters[kAlloc],
					dec ? double(counters[kAlloc]) / double(dec) : 0.0,
					(unsigned long long)counters[kFree],
					(unsigned long long)counters[kRealloc]);
	if(calls[kArenaRestore] || calls[kArenaPush])
		std::printf("  pages : %.1f/Restore (%llu appels)  %.1f/Push (%llu appels)\n",
					calls[kArenaRestore]
						? double(counters[kPagesRestored]) / double(calls[kArenaRestore])
						: 0.0,
					(unsigned long long)calls[kArenaRestore],
					calls[kArenaPush]
						? double(counters[kPagesPushed]) / double(calls[kArenaPush])
						: 0.0,
					(unsigned long long)calls[kArenaPush]);
}

} // namespace

void Enable() {
	g_tsc0 = __rdtsc();
	g_t0 = std::chrono::steady_clock::now();
	enabled = true;
}

void FlushThread() {
	if(enabled)
		FlushBuf(Tl());
}

void CountSlow(uint32_t counter, uint64_t n) {
	Tl().counters[counter] += n;
}

void Scope::Begin() {
	TlBuf& b = Tl();
	buf_ = &b;
	child0_ = b.child;
	t0_ = __rdtsc();
}

void Scope::End() {
	const uint64_t dt = __rdtsc() - t0_;
	TlBuf& b = *static_cast<TlBuf*>(buf_);
	++b.calls[site_];
	const uint64_t inner = b.child - child0_;
	b.ticks[site_] += dt > inner ? dt - inner : 0;
	b.child = child0_ + dt;
}

void PrintPhase(const char* label) {
	if(!enabled)
		return;
	FlushThread();
	uint64_t calls[kSiteCount], ticks[kSiteCount], counters[kCounterCount];
	for(uint32_t i = 0; i < kSiteCount; ++i) {
		calls[i] = g_calls[i].exchange(0, std::memory_order_relaxed);
		ticks[i] = g_ticks[i].exchange(0, std::memory_order_relaxed);
		g_run_calls[i] += calls[i];
		g_run_ticks[i] += ticks[i];
	}
	for(uint32_t i = 0; i < kCounterCount; ++i) {
		counters[i] = g_counters[i].exchange(0, std::memory_order_relaxed);
		g_run_counters[i] += counters[i];
	}
	PrintTable(label, calls, ticks, counters);
}

void PrintTotal() {
	if(!enabled)
		return;
	// Une phase non imprimee (chemin sans PrintPhase) est versee au cumul ici.
	PrintPhase("phase residuelle");
	PrintTable("cumul du run", g_run_calls, g_run_ticks, g_run_counters);
}

} // namespace prof

} // namespace solver

// --- definitions du point d'accroche Lua -------------------------------------
extern "C" {
thread_local combosolver_alloc_fn combosolver_lua_alloc = nullptr;
thread_local void* combosolver_lua_alloc_ud = nullptr;
thread_local void* combosolver_lua_state = nullptr;
}

// --- surcharge globale de l'allocateur C++ -----------------------------------
//
// C'est ce qui capte les objets C++ du core (card, effect, group, unites du
// processeur, noeuds de conteneurs STL) SANS toucher une seule ligne d'ocgcore.
// La liberation est routee par plage d'adresses : elle fonctionne donc meme en
// dehors de tout ArenaScope, y compris a la destruction du duel.
namespace {

void* SolverAllocate(size_t n) {
	if(auto* a = solver::CurrentArena()) {
		if(void* p = a->Allocate(n))
			return p;
		// L'arene est pleine. On sert quand meme depuis le tas — refuser
		// planterait le core — mais l'arene est desormais EMPOISONNEE : ce que
		// ce worker mesurera ensuite porte sur un duel que Restore() ne sait
		// plus reconstituer. Le drapeau est collant et fait avorter le worker.
		a->NoteFallback();
		solver::detail::NoteHostFallback();
	}
	void* p = std::malloc(n ? n : 1);
	if(!p)
		throw std::bad_alloc();
	return p;
}

void SolverRelease(void* p) noexcept {
	if(!p)
		return;
	if(auto* a = solver::OwnerArena()) {
		if(a->Contains(p)) {
			a->Free(p);
			return;
		}
	}
	std::free(p);
}

} // namespace

void* operator new(size_t n) { return SolverAllocate(n); }
void* operator new[](size_t n) { return SolverAllocate(n); }
void* operator new(size_t n, const std::nothrow_t&) noexcept {
	try { return SolverAllocate(n); } catch(...) { return nullptr; }
}
void* operator new[](size_t n, const std::nothrow_t&) noexcept {
	try { return SolverAllocate(n); } catch(...) { return nullptr; }
}
void operator delete(void* p) noexcept { SolverRelease(p); }
void operator delete[](void* p) noexcept { SolverRelease(p); }
void operator delete(void* p, size_t) noexcept { SolverRelease(p); }
void operator delete[](void* p, size_t) noexcept { SolverRelease(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { SolverRelease(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { SolverRelease(p); }

// Sur-alignement (> 16 octets) : l'arene ne le garantit pas, on laisse ces
// allocations a la CRT. Elles sortent donc de l'instantane — le compteur de
// repli permet de verifier qu'ocgcore n'en fait aucune.
void* operator new(size_t n, std::align_val_t al) {
	if(static_cast<size_t>(al) <= 16)
		return SolverAllocate(n);
	if(auto* a = solver::CurrentArena()) {
		// Meme consequence qu'un debordement : cet objet vit hors de
		// l'instantane et Restore() ne le retablira pas (C7).
		a->NoteFallback();
		solver::detail::NoteHostFallback();
	}
#if defined(R2V_WASM)
	// aligned_alloc exige une taille multiple de l'alignement (C11) ; la CRT
	// Windows ne l'exige pas. On arrondit.
	const size_t a = static_cast<size_t>(al);
	void* p = std::aligned_alloc(a, ((n ? n : 1) + a - 1) & ~(a - 1));
#else
	void* p = _aligned_malloc(n ? n : 1, static_cast<size_t>(al));
#endif
	if(!p)
		throw std::bad_alloc();
	return p;
}
void* operator new[](size_t n, std::align_val_t al) { return operator new(n, al); }
void operator delete(void* p, std::align_val_t) noexcept {
	if(!p)
		return;
	if(auto* a = solver::OwnerArena()) {
		if(a->Contains(p)) {
			a->Free(p);
			return;
		}
	}
#if defined(R2V_WASM)
	std::free(p);
#else
	_aligned_free(p);
#endif
}
void operator delete[](void* p, std::align_val_t al) noexcept { operator delete(p, al); }
void operator delete(void* p, size_t, std::align_val_t al) noexcept { operator delete(p, al); }
void operator delete[](void* p, size_t, std::align_val_t al) noexcept { operator delete(p, al); }
