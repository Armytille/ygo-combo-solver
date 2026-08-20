#include "arena.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#if defined(__EMSCRIPTEN__)
// --- WEBASSEMBLY BACKING ----------------------------------------------------
//
// Three things are missing, and only one of them is hard.
//
//   1. VirtualAlloc/VirtualFree. The wasm linear memory is ONE contiguous block
//      that never moves: a single malloc at startup satisfies, for free, the
//      arena's central invariant ("restore at the SAME base address"). Lazy
//      reservation disappears, so `reserve` becomes a real commit and has to be
//      sized against the observed high-water mark (--arena-mb) rather than
//      against address space. The existing escape counter is the safety net.
//   2. _BitScanForward64 / __rdtsc. Direct substitutions.
//   3. GetWriteWatch. NO equivalent: no mprotect, no page fault handler, no
//      dirty bits (the memory-control proposal has not shipped). This is the
//      only hard blocker of the port, and it is handled by a software WRITE
//      BARRIER; see `barrier` below.
#include <atomic>
#include <emscripten.h>
#include <cstdlib>
#define R2V_WASM 1
// There is no cycle counter under wasm. `emscripten_get_now()` returns
// milliseconds, so we count in NANOSECONDS, which drops TscGhz() to 1.0 and
// leaves the whole profile table correct without touching it.
//
// CAUTION: this is a call into JS. Under node it is nanosecond-resolution and
// cheap; in the browser, `performance.now()` is CLAMPED TO 5 us in a
// cross-origin isolated context, so under isolation the --profile table no
// longer means anything. In the browser, judge by wall time and exact counters.
static inline unsigned long long r2v_tsc_ns() {
	return static_cast<unsigned long long>(emscripten_get_now() * 1e6);
}
#define __rdtsc() r2v_tsc_ns()
#else
#include <intrin.h>   // _BitScanForward64
#include <windows.h>
#endif

// Hook declared by the lua/luaconf-customize.h patch. This is where the whole
// Lua heap moves into the arena.
#include "luaconf-customize.h"

#if defined(R2V_WASM)
// --- WRITE BARRIER ---------------------------------------------------------
//
// Replaces hardware dirty-page tracking. Two simpler fallbacks fail, and by
// BANDWIDTH, not by CPU:
//   - full copy of the served region: x21.4 on bytes, 4.63 TB in 26 s;
//   - detection by comparison against the mirror: 10.8 TB read, same wall.
// Here the traffic stays what the native build pays; we only spend
// instructions.
//
// The bitmap covers ALL of linear memory and is indexed by ABSOLUTE page
// number. Consequence: the barrier has NEITHER a range test NOR a branch. A
// store to the C stack sets a bit nobody reads, since SyncDirty only sweeps the
// arena's slice. That is what makes it cheap enough to sit in front of every
// write from ocgcore and from Lua.
//
// The instrumentation comes from clang (-fsanitize-coverage=trace-stores),
// applied to the ocgcore and Lua translation units ONLY. Memory intrinsics
// (memcpy, memset, memmove) are NOT instrumented by that pass: they are
// intercepted at link time (-Wl,--wrap=) and marked by hand. Correctness is
// judged by the harness already in place: the restore fidelity test and the
// snapshot stress test.
namespace barrier {

constexpr size_t kPageShift = 12;
constexpr size_t kPageSize = size_t(1) << kPageShift;
// Coverage: ALL of the wasm32 address space (4 GiB). At 4 KB per page and one
// bit per page that is 2^20 pages, i.e. 128 KB of bitmap per thread, allocated
// once, never reallocated, cache-resident. Covering the whole space is what
// lets the barrier skip both the range test and the branch: a store outside the
// arena sets a bit nobody reads.
//
// 32-BIT TRAP: writing `size_t(4) << 30` yields ZERO here, because size_t is 32
// bits under wasm32. The bitmap became an empty array and every mark wrote out
// of bounds. So we count in PAGES, never in bytes.
constexpr size_t kPages = size_t(1) << (32 - kPageShift);
constexpr size_t kWords = kPages >> 6;

// NO `thread_local uint64_t bits[kWords]`: 128 KB of TLS per thread breaks
// emscripten's threaded build (measured: -fsanitize-coverage alone, or
// -mno-bulk-memory-opt alone, is then enough to crash, because the TLS image is
// copied by bulk instructions when a thread is born). So TLS holds only a
// POINTER; the bitmap lives on the heap, one per thread, allocated by
// Arena::Init on the owning thread.
//
// `dummy` is the target for threads that have no arena: marking there has no
// effect and nobody reads it, which avoids a null test in the barrier, exactly
// where a branch would cost the most.
alignas(64) uint64_t dummy[kWords];
thread_local uint64_t* bits = dummy;

#define R2V_NOCOV __attribute__((no_sanitize("coverage")))

R2V_NOCOV inline void MarkPage(uintptr_t a) {
	const size_t page = a >> kPageShift;
	bits[page >> 6] |= uint64_t(1) << (page & 63);
}

// Gives the current thread its own bitmap. Idempotent.
void EnsureBits() {
	if(bits != dummy)
		return;
	if(void* p = std::calloc(kWords, sizeof(uint64_t)))
		bits = static_cast<uint64_t*>(p);
}

// For bulk writes, which the instrumentation does not see.
R2V_NOCOV void MarkRange(const void* p, size_t n) {
	if(!n)
		return;
	uintptr_t a = reinterpret_cast<uintptr_t>(p);
	const uintptr_t end = a + n - 1;
	for(a &= ~(kPageSize - 1); a <= end; a += kPageSize)
		MarkPage(a);
}

// --- ARENA REGISTRY (GLOBAL variables, emphatically not thread_local) -------
//
// The memcpy/memset hooks are installed over the WHOLE program, including
// `__wasm_init_tls`, which copies the TLS block of a newborn thread... with
// memcpy. And `bits` LIVES in that TLS block. Marking from the hook at that
// moment writes through a TLS block that is not in place yet, and that is
// exactly what crashes the search (replay creates no thread, so the bug is
// invisible to the fidelity test).
//
// So the hook touches TLS ONLY when the destination falls inside an arena.
// Two-stage filter: the envelope of all live arenas (two comparisons, never any
// TLS), then, on a hit only, the exact registry. An address can be inside the
// envelope without being in any arena, and a TLS block could sit precisely
// there.
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
	// The envelope only grows: shrinking it would be a race.
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

// Analogue of ResetWriteWatch(base, committed): clears the slice of the bitmap
// covering the arena. Called after a restore, otherwise the writes made by the
// restore ITSELF would accumulate (the arena is then identical to the mirror:
// nothing in it is dirty).
R2V_NOCOV void ClearSlice(uintptr_t base_addr, size_t bytes) {
	const size_t first = (base_addr >> kPageShift) >> 6;
	const size_t words = (bytes >> kPageShift) / 64 + 2;
	for(size_t w = 0; w < words; ++w)
		bits[first + w] = 0;
}

} // namespace barrier

#if !defined(R2V_NO_BARRIER)
extern "C" {
// Callbacks from -fsanitize-coverage=trace-stores. Defined here so LTO can fold
// them into the instrumented code: without inlining, the call costs more than
// the marking itself.
R2V_NOCOV void __sanitizer_cov_store1(void* p) { barrier::MarkPage(reinterpret_cast<uintptr_t>(p)); }
R2V_NOCOV void __sanitizer_cov_store2(void* p) { barrier::MarkPage(reinterpret_cast<uintptr_t>(p)); }
R2V_NOCOV void __sanitizer_cov_store4(void* p) { barrier::MarkPage(reinterpret_cast<uintptr_t>(p)); }
R2V_NOCOV void __sanitizer_cov_store8(void* p) { barrier::MarkPage(reinterpret_cast<uintptr_t>(p)); }
R2V_NOCOV void __sanitizer_cov_store16(void* p) { barrier::MarkPage(reinterpret_cast<uintptr_t>(p)); }
// A 16-byte write can straddle two pages; the other sizes are aligned by the
// compiler and never straddle.
R2V_NOCOV void __sanitizer_cov_storeN(void* p, size_t n) { barrier::MarkRange(p, n); }

// Interception of bulk writes (see --wrap in the wasm build script).
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
// --- LOWERING OF BULK OPERATIONS (wasm-opt) --------------------------------
//
// With threads, `bulk-memory` is mandatory, and clang then lowers STRUCTURE
// copies to a `memory.copy` instruction, invisible to -fsanitize-coverage and
// to --wrap alike. The verifier quantifies the hole: 2 pages per run, and they
// are pointer arrays, i.e. real corruption.
//
// `wasm-opt --llvm-memory-copy-fill-lowering` replaces those instructions with
// CALLS to `__memory_copy` / `__memory_fill`. By DEFINING them here we take
// back control of the last writes that still escaped.
R2V_NOCOV void __memory_copy(void* d, const void* s, size_t n) {
	if(barrier::InAnyArena(d))
		barrier::MarkRange(d, n);
	__real_memmove(d, s, n);   // memmove: the source may overlap
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

// A span is the unit of carving: 64 KB cut into blocks of a single size class.
// The class follows from the address, which avoids a per-allocation header: 16
// bytes of header would double the cost of the small Lua objects, which are
// typically 24 to 64 bytes.
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

// Two distinct pointers: `active` follows the scopes and decides WHERE to
// allocate; `owner` stays valid as long as the arena exists and routes frees.
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
	(void)preferred_base;   // a single linear memory: the base is what it is
	page_size = barrier::kPageSize;
	reserve = (reserve + kSpanSize - 1) & ~(kSpanSize - 1);
	// Aligned on 64 pages: the barrier bitmap slice then falls on a WORD boundary,
	// and SyncDirty pours it in with no shift.
	constexpr size_t kBaseAlign = barrier::kPageSize * 64;
	base = static_cast<uint8_t*>(std::aligned_alloc(kBaseAlign,
					(reserve + kBaseAlign - 1) & ~(kBaseAlign - 1)));
	write_watch = base != nullptr;   // the barrier plays the role of the hardware tracker
	if(!base) {
		error = "aligned_alloc a echoue pour " + std::to_string(reserve) + " octets";
		return false;
	}
	barrier::EnsureBits();
	// Makes the range known to the memory hooks: without it they cannot tell,
	// WITHOUT touching TLS, whether a bulk copy targets the arena.
	barrier::Register(reinterpret_cast<uintptr_t>(base),
					  reinterpret_cast<uintptr_t>(base) + reserve);
#else
	SYSTEM_INFO si{};
	GetSystemInfo(&si);
	page_size = si.dwPageSize;
	reserve = (reserve + kSpanSize - 1) & ~(kSpanSize - 1);

	// MEM_WRITE_WATCH is not used for restoring yet, but it makes it possible to
	// count the pages an action dirties, the figure that decides whether an
	// incremental restore is worth it.
	auto reserve_at = [&](LPVOID at, bool watch) -> uint8_t* {
		DWORD flags = MEM_RESERVE | (watch ? MEM_WRITE_WATCH : 0u);
		return static_cast<uint8_t*>(VirtualAlloc(at, reserve, flags, PAGE_READWRITE));
	};

	base = reserve_at(reinterpret_cast<LPVOID>(preferred_base), true);
	write_watch = base != nullptr;
	if(!base) {
		// The requested address may be taken. Inside one process the fixed base is
		// not necessary: it only becomes so to carry a snapshot from one process to
		// another.
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

	// TRAP: these containers belong to the allocator itself. Letting them grow
	// while the arena is live would make them allocate INSIDE the arena they
	// administer, and a restore would then overwrite their contents from under
	// their own feet. They are sized here, once and for all, while no arena is
	// live yet: no reallocation afterwards. Same reason for the dirty page buffer.
	free_span_runs.reserve(spans.size() + 1);
	dirty_scratch.reserve(reserve / page_size + 1);

	t_owner = this;   // after the reservations, otherwise they would be rerouted
	return true;
}

void Arena::Shutdown() {
	// The containers below live outside the arena, but we cut the routing before
	// destroying them so that no free is rerouted any more.
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
	// Nothing to ask the system for: the range is already ours. `committed` stays
	// a HIGH-WATER MARK; it bounds the mirror and SyncDirty's sweep, so it keeps
	// its full meaning. The page is zeroed so the mirror starts from defined
	// contents (VirtualAlloc(MEM_COMMIT) did that for free).
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

// CLOSING THE `memory.copy` HOLE (threaded wasm).
//
// With threads, `bulk-memory` is mandatory, so clang lowers STRUCTURE copies to
// a `memory.copy` instruction that neither -fsanitize-coverage nor --wrap sees.
// The verifier's finding: 2 pages escaped on every run.
//
// But those copies almost always target a block that was JUST ALLOCATED. So we
// mark the block at allocation time: any later write, seen by the
// instrumentation or not, then lands on an already-dirty page. Over-marking is
// always SAFE (it copies one page too many, never one too few) and it costs a
// handful of instructions per allocation.
//
// What remains outside, a bulk copy into an ALREADY OLD object, is what the
// verifier must keep reporting as zero.
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
	// Counts only, never reads a clock: ~10^6 calls/s per worker, and one rdtsc per
	// call would manufacture the slowdown it claims to observe.
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
		// No free block left: take a span and carve it whole.
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
		return; // free span or continuation of a large block: pointer not allocated here
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
	// If the new size fits in the same class, nothing moves: Lua resizes its
	// buffers in small increments, so this case is frequent.
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
	// Fresh pages have never been photographed: align them on the arena, otherwise
	// a restore would write anything at all into them.
	std::memcpy(mirror.data() + old, base + old, committed - old);
}

size_t Arena::SyncDirty() {
	if(!write_watch || !committed)
		return 0;
#if defined(R2V_WASM) && defined(R2V_NO_BARRIER)
	// CONTROL ARM. With no barrier we know nothing: everything served is declared
	// dirty. That is CORRECT (it is the fallback already planned for when
	// GetWriteWatch fails) and it is the arm the barrier is measured against.
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
	// R2V_ARENA_ALLDIRTY=1: SAME binary, full dirty set. It is the only way to
	// separate "the barrier is incomplete" from "something else is broken" without
	// changing a single line of generated code.
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
	// Pours the slice of the barrier bitmap covering the arena into the current
	// level, AND zeroes it, which is the semantics of
	// GetWriteWatch(WRITE_WATCH_FLAG_RESET). Since the base is aligned on 64
	// pages, the slice starts on a word boundary: no shift.
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
		// On failure we treat everything as dirty: correct, just slow.
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
				// WHAT changed, not only WHERE: text points at an uninstrumented libc
				// function, pointers at a structure copy lowered to `memory.copy`.
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
	// Spans born after the resume point go back to pristine.
	for(size_t i = cp.next_span; i < next_span; ++i)
		spans[i] = SpanInfo{};
	free_lists = cp.free_lists;
	free_span_runs = cp.free_span_runs;
	next_span = cp.next_span;
	live_bytes = cp.live_bytes;
}

// Walks the pages marked in a bitmap.
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
	// The snapshot buffers belong to the host: without this pause they would be
	// allocated in the very arena they are supposed to photograph.
	ArenaPause off;
	EnsureMirror();
	SyncDirty();

	size_t pages = 0;
	if(!checkpoints.empty()) {
		// The current level becomes the parent: we build it the log that will let the
		// mirror be wound back when the child is popped.
		Checkpoint& parent = checkpoints.back();
		parent.undo_pages.clear();
		parent.undo_data.clear();
		ForEachDirtyPage(parent.dirty, [&](size_t page) {
			size_t off_p = page * page_size;
			if(off_p + page_size > mirror.size())
				return;
			// Two operations with DIFFERENT bounds; do not conflate them.
			//
			// The before-image only makes sense for pages that already existed when the
			// parent was pushed: past that, the span was not allocated and its contents
			// do not have to be restored.
			if(off_p < parent.in_use) {
				parent.undo_pages.push_back(uint32_t(page));
				size_t at = parent.undo_data.size();
				parent.undo_data.resize(at + page_size);
				std::memcpy(parent.undo_data.data() + at, mirror.data() + off_p,
							page_size);
			}
			// The mirror, on the other hand, must reflect the child's state WITHOUT
			// exception: that is where a restore of the child takes its pages from. A
			// page born during the parent's period would otherwise be read stale.
			std::memcpy(mirror.data() + off_p, base + off_p, page_size);
			++pages;
		});
	} else {
		// First level: the mirror must reflect the whole arena.
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
	// BARRIER VERIFIER (R2V_ARENA_VERIFY=1).
	//
	// The software barrier cannot be proven by reading: clang can lower a structure
	// copy to `memory.copy`, which neither -fsanitize-coverage nor --wrap sees. So
	// we do not ASSUME it is complete, we require it and we measure it: any page
	// that differs from the mirror without being marked is a page Restore() would
	// not put back, i.e. silent corruption.
	//
	// Cost: one memcmp of the served region per restore. Diagnostics only.
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
	// The arena is identical to the mirror again: nothing has moved since this
	// level was pushed. We restart from a pristine tracker, otherwise the pages
	// written by the restore itself would accumulate.
	std::fill(cp.dirty.begin(), cp.dirty.end(), 0);
#if defined(R2V_WASM)
	// The arena has just been put back to the mirror's state. The writes the
	// restore made ITSELF have set bits: they must be dropped, exactly as
	// ResetWriteWatch does on the Windows side.
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
	// The inner Restore() charges its own probe; the self time of arena Pop is the
	// dirty page merge and the mirror rollback.
	prof::Scope ps(prof::kArenaPop);
	// State of the child when it was pushed, then removal of the level.
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
	// What the child modified has also moved since the parent was pushed.
	if(parent.dirty.size() < child_dirty.size())
		parent.dirty.resize(child_dirty.size(), 0);
	for(size_t i = 0; i < child_dirty.size(); ++i)
		parent.dirty[i] |= child_dirty[i];

	// The mirror must go back to the parent's state.
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
	// The union of the pages modified since the target level was pushed: its own
	// period plus those of the k popped levels. (The upward merge of sequential
	// Pops builds exactly this set, one level at a time; here we take it in one
	// go.)
	std::vector<uint64_t> uni = checkpoints[target].dirty;
	for(size_t i = target + 1; i < checkpoints.size(); ++i) {
		const std::vector<uint64_t>& d = checkpoints[i].dirty;
		if(uni.size() < d.size())
			uni.resize(d.size(), 0);
		for(size_t w = 0; w < d.size(); ++w)
			uni[w] |= d[w];
	}
	// Wind the MIRROR back to the state it had when the target level was pushed:
	// level i's log takes the mirror from its child's push back to its own, so the
	// logs are applied top-down and each one is consumed.
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
	// The arena from the mirror, each page ONCE. The bound is the in_use of the
	// TARGET level: past that, the spans do not exist at that level.
	// RestoreMetadata makes them pristine, and Allocate rebuilds their chains by
	// recarving them before any use (residual contents are never read).
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
	// The arena has just been put back to the mirror's state. The writes the
	// restore made ITSELF have set bits: they must be dropped, exactly as
	// ResetWriteWatch does on the Windows side.
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
	// Must go through SyncDirty: reading the hardware tracker resets it, so a read
	// that did not pour the bits into the current level would lose them and the
	// restore would be incomplete.
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
	// MEMBER counter, not thread_local: escapes happen in the workers and this
	// summary is read from the main thread.
	s.host_fallbacks = fallbacks.load(std::memory_order_relaxed);
	s.poisoned = poisoned.load(std::memory_order_relaxed);
	return s;
}

// --- hot path profile (--profile) --------------------------------------------

namespace prof {

bool enabled = false;

namespace {

const char* const kSiteNames[kSiteCount] = {
	"recherche (reste)",     // kSearch: self time of the Run* bodies
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

// Per-thread counters, poured into the global atomics by the destructor (thread
// death; workers are joined per phase) or by FlushThread (main thread). No
// atomic at all on the per-call path.
struct TlBuf {
	uint64_t calls[kSiteCount] = {};
	uint64_t ticks[kSiteCount] = {};   // EXCLUSIVE (self) time
	uint64_t counters[kCounterCount] = {};
	// Exclusive time scheme: sum of the INCLUSIVE durations of closed scopes. An
	// enclosing scope reads what its children consumed during its own period
	// (child - child0) and subtracts it from its own time; when it closes it
	// credits its inclusive duration to its parent.
	uint64_t child = 0;
	~TlBuf() { FlushBuf(*this); }
};

std::atomic<uint64_t> g_calls[kSiteCount];
std::atomic<uint64_t> g_ticks[kSiteCount];
std::atomic<uint64_t> g_counters[kCounterCount];

// Whole-run total, fed on every phase reset.
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

// The tsc frequency is calibrated against the wall clock over the WHOLE span
// elapsed since Enable(): free, and the more accurate the longer the run.
// long.
double TscGhz() {
	const double s = std::chrono::duration<double>(
						 std::chrono::steady_clock::now() - g_t0).count();
	if(s <= 0)
		return 3.0;   // survival value, never reached in practice
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
	// Sorted by decreasing time: the profile reads top to bottom.
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
	// A phase never printed (a path with no PrintPhase) is poured into the total here.
	PrintPhase("phase residuelle");
	PrintTable("cumul du run", g_run_calls, g_run_ticks, g_run_counters);
}

} // namespace prof

} // namespace solver

// --- Lua hook definitions ----------------------------------------------------
extern "C" {
thread_local combosolver_alloc_fn combosolver_lua_alloc = nullptr;
thread_local void* combosolver_lua_alloc_ud = nullptr;
thread_local void* combosolver_lua_state = nullptr;
}

// --- global C++ allocator overload -------------------------------------------
//
// This is what captures the core's C++ objects (card, effect, group, processor
// units, STL container nodes) WITHOUT touching a single line of ocgcore. Frees
// are routed by address range, so they work even outside any ArenaScope,
// including when the duel is destroyed.
namespace {

void* SolverAllocate(size_t n) {
	if(auto* a = solver::CurrentArena()) {
		if(void* p = a->Allocate(n))
			return p;
		// The arena is full. We serve from the heap anyway, since refusing would crash
		// the core, but the arena is now POISONED: whatever this worker measures next
		// describes a duel Restore() can no longer reconstitute. The flag is sticky
		// and aborts the worker.
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

// Over-alignment (> 16 bytes): the arena does not guarantee it, so those
// allocations are left to the CRT. They therefore fall outside the snapshot,
// and the escape counter is what verifies that ocgcore makes none.
void* operator new(size_t n, std::align_val_t al) {
	if(static_cast<size_t>(al) <= 16)
		return SolverAllocate(n);
	if(auto* a = solver::CurrentArena()) {
		// Same consequence as an overflow: this object lives outside the snapshot and
		// Restore() will not put it back.
		a->NoteFallback();
		solver::detail::NoteHostFallback();
	}
#if defined(R2V_WASM)
	// aligned_alloc requires a size that is a multiple of the alignment (C11); the
	// Windows CRT does not. We round up.
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
