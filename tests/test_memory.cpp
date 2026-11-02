// Matted Engine - tests: memory allocators (Arena, ScopedMark, scratch)
#include "test_framework.h"

#include "engine/core/memory/allocators.h"

#include <thread>

using namespace mtt;
using namespace mtt::mem;

MTT_TEST(arena_alignment_and_bump) {
    Arena arena(4096);

    u8* p1 = static_cast<u8*>(arena.alloc(3));            // default 16-byte align
    MTT_CHECK(reinterpret_cast<uintptr_t>(p1) % 16 == 0, "default alignment is 16");
    p1[0] = 1; p1[1] = 2; p1[2] = 3;                      // writable

    u8* p2 = static_cast<u8*>(arena.alloc(1, 64));        // explicit 64-byte align
    MTT_CHECK(reinterpret_cast<uintptr_t>(p2) % 64 == 0, "explicit alignment is 64");

    // bump allocator: next allocation never overlaps the previous one
    MTT_CHECK(p2 >= p1 + 16, "p2 after p1 (3 bytes rounded to 16)");

    double* arr = arena.alloc_array<double>(100);
    MTT_CHECK(reinterpret_cast<uintptr_t>(arr) % alignof(double) == 0, "alloc_array respects alignof");
    for (int i = 0; i < 100; ++i) arr[i] = i * 0.5;       // writable
    MTT_NEAR(arr[99], 49.5, 1e-9);

    u8* p3 = static_cast<u8*>(arena.alloc(10));
    MTT_CHECK(p3 >= p2 + 64, "p3 after p2 (1 byte rounded to 64)");
}

MTT_TEST(arena_grows_across_blocks) {
    Arena arena(1 << 20); // 1 MiB first block

    // single oversized allocation forces a dedicated larger block
    const size_t big = 3u << 20;
    u8* p = static_cast<u8*>(arena.alloc(big));
    MTT_CHECK(p != nullptr, "oversized alloc succeeds");
    p[0] = 0xAA;
    p[big - 1] = 0xBB;
    MTT_CHECK(p[0] == 0xAA && p[big - 1] == 0xBB, "oversized block writable at both ends");

    // many small allocations spanning several blocks stay valid (blocks chain)
    constexpr int kCount = 40000;
    u8* ptrs[kCount];
    for (int i = 0; i < kCount; ++i) {
        ptrs[i] = static_cast<u8*>(arena.alloc(32));
        ptrs[i][0] = static_cast<u8>(i);
        ptrs[i][31] = static_cast<u8>(i ^ 0xFF);
    }
    int bad = 0;
    for (int i = 0; i < kCount; ++i)
        if (ptrs[i][0] != static_cast<u8>(i) || ptrs[i][31] != static_cast<u8>(i ^ 0xFF)) ++bad;
    MTT_EQ(bad, 0);
}

MTT_TEST(arena_reset_and_scoped_mark) {
    Arena arena(4096);
    arena.alloc(1000);
    arena.alloc(3000);
    arena.alloc(5000); // forces a second block
    arena.reset();     // drops all but the last block, rewinds

    // after reset the arena is usable again
    u8* p = static_cast<u8*>(arena.alloc(100, 16));
    MTT_CHECK(p != nullptr, "alloc after reset works");
    MTT_CHECK(reinterpret_cast<uintptr_t>(p) % 16 == 0, "alignment preserved after reset");
    p[0] = 7; p[99] = 7;
    MTT_CHECK(p[0] == 7 && p[99] == 7, "post-reset block writable");

    // ScopedMark rewinds everything allocated inside the scope
    {
        ScopedMark mark(arena);
        arena.alloc(2048);
        arena.alloc(2048);
        u8* inside = static_cast<u8*>(arena.alloc(8));
        inside[0] = 42;
    }
    u8* after = static_cast<u8*>(arena.alloc(16));
    MTT_CHECK(after != nullptr, "alloc after ScopedMark works");
    after[0] = 9;
    MTT_CHECK(after[0] == 9, "post-scope block writable");

    // free_all in the destructor must not leak/crash (checked with runtime + ASan runs)
    arena.free_all();
    u8* fresh = static_cast<u8*>(arena.alloc(8));
    MTT_CHECK(fresh != nullptr, "alloc after free_all works");
}

MTT_TEST(scratch_is_per_thread) {
    Arena& s1 = scratch();
    s1.alloc(64);

    Arena& s2 = scratch();
    MTT_CHECK(&s1 == &s2, "same thread returns the same scratch arena");

    Arena* worker_scratch = nullptr;
    std::thread t([&] { worker_scratch = &scratch(); });
    t.join();
    MTT_CHECK(worker_scratch != nullptr && worker_scratch != &s1, "worker thread gets its own scratch");

    // ScratchScope rewinds the thread's scratch on scope exit
    {
        ScratchScope scope;
        scratch().alloc(100);
    }
    u8* p = static_cast<u8*>(scratch().alloc(32));
    MTT_CHECK(p != nullptr, "scratch usable after ScratchScope");
    p[0] = 5;
}
