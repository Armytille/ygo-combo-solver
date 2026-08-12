#include "arena.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <new>

#include <intrin.h>   // _BitScanForward64
#include <windows.h>

// Point d'accroche declare par le patch lua/luaconf-customize.h. C'est par la
// que tout le heap Lua bascule dans l'arene.
#include "luaconf-customize.h"

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
size_t ArenaHostFallbacks() { return t_host_fallbacks; }

namespace detail {
void NoteHostFallback() { ++t_host_fallbacks; }
} // namespace detail

ArenaScope::ArenaScope(Arena* a) : previous(t_active) { t_active = a; }
ArenaScope::~ArenaScope() { t_active = previous; }

ArenaPause::ArenaPause() : previous(t_active) { t_active = nullptr; }
ArenaPause::~ArenaPause() { t_active = previous; }

bool Arena::Init(size_t reserve, std::uintptr_t preferred_base, std::string& error) {
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
		VirtualFree(base, 0, MEM_RELEASE);
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
	if(!VirtualAlloc(base + committed, want - committed, MEM_COMMIT, PAGE_READWRITE))
		return false;
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

void* Arena::Allocate(size_t size) {
	if(size == 0)
		size = 1;
	++alloc_count;

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
	return p;
}

void Arena::Free(void* ptr) {
	if(!ptr || !Contains(ptr))
		return;
	++free_count;
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
}

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
			unsigned long b;
			_BitScanForward64(&b, word);
			word &= word - 1;
			fn((w << 6) + b);
		}
	}
}

void Arena::Push() {
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
}

void Arena::Restore() {
	if(checkpoints.empty())
		return;
	ArenaPause off;
	SyncDirty();
	Checkpoint& cp = checkpoints.back();
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
	if(write_watch && committed)
		ResetWriteWatch(base, committed);
	last_restore = { pages, pages * page_size };
}

void Arena::Pop() {
	if(checkpoints.empty())
		return;
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
	s.host_fallbacks = t_host_fallbacks;
	return s;
}

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
	if(solver::CurrentArena())
		solver::detail::NoteHostFallback();
	void* p = _aligned_malloc(n ? n : 1, static_cast<size_t>(al));
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
	_aligned_free(p);
}
void operator delete[](void* p, std::align_val_t al) noexcept { operator delete(p, al); }
void operator delete(void* p, size_t, std::align_val_t al) noexcept { operator delete(p, al); }
void operator delete[](void* p, size_t, std::align_val_t al) noexcept { operator delete(p, al); }
