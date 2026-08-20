// Snapshottable memory arena for an ocgcore duel.
//
// WHY
// The OCG API offers no state cloning, and a hand-written serialiser is out of
// reach: at a MSG_SELECT_*, Lua coroutines sit suspended in the middle of an
// effect resolution, with their stacks and their upvalues (interpreter.cpp,
// call_coroutine). So we do not save the game, we save THE MEMORY.
//
// HOW
// All the duel's mutable memory is confined to an address range we control:
//   - the Lua heap, through the allocator passed to lua_newstate (luaconf patch);
//   - the core's C++ objects, through the global operator new/delete overload.
// The restore happens AT THE SAME BASE ADDRESS, so every absolute pointer stays
// valid with no relocation: the duel never knows it was restored.
//
// IMPLEMENTATION STATUS
// The snapshot is a full copy of the served region: simple, safe, and already
// usable given the measured cost of one engine step (0.18 ms). Dirty-page
// tracking is wired as INSTRUMENTATION only; it measures how many pages an
// action dirties, the figure that decides whether an incremental restore pays.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace solver {

struct ArenaStats {
	size_t reserved = 0;       // reserved address range
	size_t committed = 0;      // actually committed to the OS
	size_t in_use = 0;         // top of the served region (snapshot bound)
	size_t live_bytes = 0;     // sum of allocated blocks not yet freed
	size_t alloc_count = 0;
	size_t free_count = 0;
	size_t host_fallbacks = 0; // allocations that escaped the arena while it
							   // was live (must stay at zero)
	bool poisoned = false;     // at least one escape: state is no longer captured
};

// One arena per thread: each worker explores its own subtree, and snapshots
// never travel between threads (different bases).
class Arena {
public:
	// `reserve` is address space, not memory: only what is touched is committed.
	// The working set must stay small enough to sit in L3.
	bool Init(size_t reserve, std::uintptr_t preferred_base, std::string& error);
	void Shutdown();
	bool Ready() const { return base != nullptr; }

	void* Allocate(size_t size);
	void Free(void* ptr);
	void* Reallocate(void* ptr, size_t old_size, size_t new_size);

	// --- ESCAPES -------------------------------------------------------------
	//
	// An allocation that did not fit in the arena goes to the host heap.
	// `Restore()` CANNOT restore it: from that moment on the duel's state diverges
	// from what the search believes it restored, and everything that worker
	// measures afterwards (nodes, board keys, solutions) describes a corrupt duel.
	// So this is not a statistic, it is a stop condition.
	//
	// It is a member, atomic, and the flag is STICKY. A `thread_local` read from
	// the MAIN thread would make worker escapes structurally invisible and print
	// "none: all state is captured" by construction.
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

	// --- snapshots (LIFO stack, which is what a depth-first walk needs)
	//
	// Only the pages actually modified are copied. A mirror of the state at the
	// top of the stack supplies the before-images, which page tracking does not.
	//
	// Typical DFS sequence:
	//     Push()                      once on arriving at the node
	//     for each child: advance, explore, Restore()
	//     Pop()                       on leaving the node
	// Restore() is the frequent operation, and the cheapest: it copies back only
	// what the child dirtied.
	void Push();     // opens a level: the current state becomes the resume point
	void Restore();  // returns to the resume point WITHOUT popping (iterating over children)
	void Pop();      // returns to the resume point and pops
	// Equivalent to k x Pop() then Restore(), in ONE pass: a single SyncDirty, the
	// undo logs applied to the mirror from the top down, then the arena restored
	// from the mirror over the UNION of the dirty pages. Each hot page is copied
	// once instead of once per level, and the span table is restored only at the
	// target level. Motivated by the finisher profile on a full stack, where
	// Restore+Pop account for 27 % of the time.
	void PopToAndRestore(size_t k);
	void Discard();  // pops without restoring
	size_t Depth() const { return checkpoints.size(); }

	struct CheckpointCost {
		size_t pages = 0;    // pages handled by the last operation
		size_t bytes = 0;
	};
	CheckpointCost LastPush() const { return last_push; }
	CheckpointCost LastRestore() const { return last_restore; }
	size_t UndoBytes() const;   // memory retained by the log stack

	// --- dirty page instrumentation
	void ResetDirtyTracking();
	size_t CountDirtyPages();   // pages written since the last call/reset
	bool DirtyTrackingAvailable() const { return write_watch; }

	ArenaStats Stats() const;
	size_t PageSize() const { return page_size; }
	std::uintptr_t BaseAddress() const { return base_addr; }

	// Checks the vital invariant: no allocator structure may live inside the
	// arena, otherwise a restore overwrites it from under its own feet. Returns a
	// description of the problem, empty when all is well.
	std::string SelfCheck() const;

	// Allocator in the shape lua_newstate expects.
	static void* LuaAlloc(void* ud, void* ptr, size_t osize, size_t nsize);

private:
	struct SpanInfo {
		uint8_t klass = 0xff;  // 0xff free, 0xfe head of a large block, 0xfd continuation
		uint32_t run = 0;      // number of spans, filled in on the first one
	};
	// The allocator's own state is part of the state to restore. The free-block
	// chains live in the arena, hence in the copied pages; the list heads and the
	// span table are kept outside the arena so that a restore does not overwrite
	// them from under their own feet.
	struct Checkpoint {
		size_t in_use = 0;
		// Pages modified since THIS level was pushed. Accumulated in software: the
		// hardware tracker has a single global bitset and cannot serve several levels
		// at once.
		std::vector<uint64_t> dirty;
		// The contents the pages modified during this level held when the level was
		// pushed. Used to wind the mirror back when a child is popped. Rebuilt on
		// every child Push.
		std::vector<uint32_t> undo_pages;
		std::vector<uint8_t> undo_data;

		std::vector<SpanInfo> spans;
		std::vector<void*> free_lists;
		std::vector<size_t> free_span_runs;
		size_t next_span = 0;
		size_t live_bytes = 0;
	};

	// Marks a fresh block dirty (see arena.cpp): closes the hole left by bulk
	// copies, which clang's instrumentation does not see.
	static void MarkFresh(const void* p, size_t n);
	bool CommitTo(size_t offset);
	void* AllocSpans(size_t span_count);
	void FreeSpans(size_t span_index, size_t span_count);
	void EnsureMirror();
	// Pours the hardware tracker's bits into the top level and resets the
	// hardware tracker. Returns the page count of the last sample. EVERY read of
	// the hardware tracker must go through here: reading it resets it, so a read
	// that did not pour the bits would lose them.
	size_t SyncDirty();
	void CaptureMetadata(Checkpoint& cp) const;
	void RestoreMetadata(const Checkpoint& cp);
#if defined(__EMSCRIPTEN__)
	// Hardware dirty-page tracking does not exist under wasm; it is replaced by a
	// software write barrier, which cannot be proven by reading. These three
	// REQUIRE it instead of assuming it (R2V_ARENA_VERIFY=1).
	void VerifyDirtySet(const Checkpoint& cp);
public:
	static bool VerifyBarrier();
	static void PrintVerifyReport();
private:
#endif

	uint8_t* base = nullptr;
	std::uintptr_t base_addr = 0;
	size_t reserved = 0, committed = 0, page_size = 4096;
	bool write_watch = false;

	std::vector<SpanInfo> spans;
	std::vector<void*> free_lists;
	std::vector<size_t> free_span_runs;
	size_t next_span = 0;

	std::vector<Checkpoint> checkpoints;
	// Full copy of the state at the top of the stack. It is the only possible
	// source of before-images: page tracking says WHICH pages changed, never what
	// they contained.
	std::vector<uint8_t> mirror;
	size_t live_bytes = 0, alloc_count = 0, free_count = 0;
	// Atomic: written by the owning thread, read by it AND by the main thread at
	// report time.
	std::atomic<size_t> fallbacks{ 0 };
	std::atomic<bool> poisoned{ false };
	std::vector<uint8_t*> dirty_scratch;
	CheckpointCost last_push, last_restore;
};

namespace detail {
// Reports an allocation that had to leave the arena while it was live.
void NoteHostFallback();
} // namespace detail

// Arena to allocate from right now (nullptr outside a duel or during a pause).
Arena* CurrentArena();
// The thread's arena, regardless of pauses: used to route frees, which can
// happen outside any scope.
Arena* OwnerArena();

// Makes the arena live. To be placed around calls into the core and only
// around those: host code must allocate normally.
class ArenaScope {
public:
	explicit ArenaScope(Arena* a);
	~ArenaScope();
	ArenaScope(const ArenaScope&) = delete;
	ArenaScope& operator=(const ArenaScope&) = delete;

private:
	Arena* previous;
};

// Suspends the arena inside an ArenaScope. Indispensable in our callbacks
// (script reader, error log): they are called FROM the core, so the arena is
// live, but what they allocate belongs to the host and must not be wiped by a
// restore.
class ArenaPause {
public:
	ArenaPause();
	~ArenaPause();
	ArenaPause(const ArenaPause&) = delete;
	ArenaPause& operator=(const ArenaPause&) = delete;

private:
	Arena* previous;
};

// --- HOT PATH PROFILE (--profile) ------------------------------------------
//
// Where a simulated decision spends its time. The rules, each paid once:
//   - thread_local counters, poured into global atomics when the thread DIES
//     (workers are created and joined per phase, so the pour is guaranteed)
//     and by an explicit flush for the main thread; a thread_local read from
//     another thread always measures zero;
//   - never a shared atomic on the per-call path: sixteen workers hammering
//     the same cache line would measure their own contention;
//   - __rdtsc, calibrated once against the wall clock at report time;
//   - off by default: an inactive probe costs a load plus a branch.
//
// Time measured per probe is EXCLUSIVE (self): a nested probe subtracts itself
// from the one enclosing it. The kSearch probe wraps the body of the Run*
// functions, so its self time is BY CONSTRUCTION the "everything else" line;
// without it a profile lies by omission.
//
// This lives in arena.h/arena.cpp rather than in a new file: premake evaluates
// its `files { "*.cpp" }` glob at GENERATION time, so a new file would mean
// regenerating the solution. arena.h is included by duel.h, hence visible from
// the whole hot path.
namespace prof {

enum Site : uint32_t {
	kSearch = 0,     // body of a Run* function; self time is the "everything else" line
	kPrefix,         // prefix replay (finisher roots)
	kProcess,        // Duel::Process (the core itself)
	kQuery,          // Duel::Query, buffer overload (the vector overload delegates)
	kQueryCodes,     // Duel::QueryCodes
	kProcState,      // Duel::ProcessorState
	kCount,          // Duel::Count
	kEnumerate,      // EnumerateInto
	kDigest,         // StateDigest, excluding internal queries
	kBoardKey,       // ComputeBoardKeyInto, excluding internal queries
	kAtoms,          // CollectAtoms (novelty), excluding internal queries
	kRecipe,         // RecipeDistance, excluding internal queries
	kArenaPush,
	kArenaRestore,
	kArenaPop,
	kSiteCount
};

enum Counter : uint32_t {
	kAlloc = 0,      // Arena::Allocate
	kFree,           // Arena::Free
	kRealloc,        // Arena::Reallocate
	kPagesPushed,    // pages handled by Push
	kPagesRestored,  // pages copied back by Restore
	kDecisions,      // decisions/expansions (matches ++stats.nodes)
	kCounterCount
};

// Set BEFORE the threads are created (publication rides on the launch).
extern bool enabled;

void Enable();               // turns it on and calibrates the tsc/clock origin
void FlushThread();          // pours the counters of the CALLING thread
// Pours the current thread, prints the table for THE PHASE just ended (when
// probes fired) and zeroes the phase counters. The run total keeps going.
void PrintPhase(const char* label);
void PrintTotal();           // the whole-run total, at the end of the execution

void CountSlow(uint32_t counter, uint64_t n);
inline void Count(uint32_t counter, uint64_t n = 1) {
	if(enabled)
		CountSlow(counter, n);
}

class Scope {
public:
	explicit Scope(uint32_t site) : site_(site) {
		if(enabled)
			Begin();
	}
	~Scope() {
		if(buf_)
			End();
	}
	Scope(const Scope&) = delete;
	Scope& operator=(const Scope&) = delete;

private:
	void Begin();
	void End();
	void* buf_ = nullptr;
	uint64_t t0_ = 0, child0_ = 0;
	uint32_t site_;
};

} // namespace prof

} // namespace solver
