// Matted Engine - tests: job system (parallel_for coverage, nesting, submit/wait_idle)
//
// NOTE: main() calls jobs::init() before running any test and shutdown() after
// all of them, so these tests assume the pool is live. init() is idempotent.
#include "test_framework.h"

#include "engine/core/jobs/job_system.h"

#include <atomic>
#include <memory>
#include <vector>

using namespace mtt;

MTT_TEST(jobs_init_is_idempotent) {
    const u32 before = jobs::thread_count();
    jobs::init();
    jobs::init();
    MTT_CHECK(jobs::thread_count() == before, "re-init does not spawn extra workers");
    MTT_CHECK(jobs::thread_count() >= 1, "at least one worker exists");
}

MTT_TEST(jobs_worker_index_on_main) {
    MTT_EQ(jobs::worker_index(), 0u);
}

MTT_TEST(jobs_parallel_for_exact_coverage) {
    constexpr u64 kN = 10000, kGrain = 512;
    auto flags = std::make_unique<std::atomic<u8>[]>(kN);
    for (u64 i = 0; i < kN; ++i) flags[i].store(0, std::memory_order_relaxed);

    std::atomic<u64> sum{ 0 };
    std::atomic<u32> chunk_calls{ 0 };

    jobs::parallel_for(kN, kGrain, [&](u64 begin, u64 end) {
        ++chunk_calls;
        for (u64 i = begin; i < end; ++i) {
            flags[i].fetch_add(1, std::memory_order_relaxed);
            sum.fetch_add(i, std::memory_order_relaxed);
        }
    });

    // every index visited exactly once
    int missed = 0, double_hit = 0;
    for (u64 i = 0; i < kN; ++i) {
        const u8 f = flags[i].load();
        if (f == 0) ++missed;
        else if (f > 1) ++double_hit;
    }
    MTT_EQ(missed, 0);
    MTT_EQ(double_hit, 0);
    MTT_EQ(sum.load(), kN * (kN - 1) / 2);

    // chunking: ceil(10000/512) = 20 chunks, each invoke covers one chunk
    MTT_EQ(chunk_calls.load(), 20u);
}

MTT_TEST(jobs_parallel_for_empty_and_grain_zero) {
    bool ran = false;
    jobs::parallel_for(0, 8, [&](u64, u64) { ran = true; });
    MTT_CHECK(!ran, "count == 0 never calls fn");

    // grain == 0 must clamp (to 1) and still cover everything
    std::atomic<u64> visits{ 0 };
    jobs::parallel_for(100, 0, [&](u64 begin, u64 end) { visits += (end - begin); });
    MTT_EQ(visits.load(), 100u);
}

MTT_TEST(jobs_worker_index_range_inside_parallel_for) {
    const u32 workers = jobs::thread_count();
    std::atomic<u32> mn{ 0xFFFFFFFFu }, mx{ 0 };
    jobs::parallel_for(4096, 64, [&](u64, u64) {
        const u32 w = jobs::worker_index();
        u32 prev = mn.load(std::memory_order_relaxed);
        while (w < prev && !mn.compare_exchange_weak(prev, w)) {}
        prev = mx.load(std::memory_order_relaxed);
        while (w > prev && !mx.compare_exchange_weak(prev, w)) {}
    });
    // main thread always participates in chunk processing -> index 0 is hit
    MTT_EQ(mn.load(), 0u);
    MTT_CHECK(mx.load() <= workers, "worker index never exceeds thread_count");
}

MTT_TEST(jobs_nested_parallel_for_does_not_deadlock) {
    // calling parallel_for from inside a chunk must run (inline on workers,
    // dispatched on the calling thread) and complete with full coverage.
    constexpr u64 kOuter = 64, kInner = 64;
    auto inner_hits = std::make_unique<std::atomic<u8>[]>(kInner);
    for (u64 i = 0; i < kInner; ++i) inner_hits[i].store(0, std::memory_order_relaxed);

    std::atomic<u32> outer_chunks{ 0 };
    jobs::parallel_for(kOuter, 8, [&](u64 begin, u64 end) {
        ++outer_chunks;
        jobs::parallel_for(kInner, 8, [&](unsigned long long ib, unsigned long long ie) {
            for (unsigned long long i = ib; i < ie; ++i)
                inner_hits[i].fetch_add(1, std::memory_order_relaxed);
        });
    });

    MTT_EQ(outer_chunks.load(), 8u); // 64/8
    for (u64 i = 0; i < kInner; ++i)
        MTT_CHECK(inner_hits[i].load() == kOuter, "inner index i hit once per outer chunk");
}

MTT_TEST(jobs_submit_and_wait_idle) {
    std::atomic<int> counter{ 0 };
    for (int i = 0; i < 100; ++i)
        jobs::submit([&counter] { counter.fetch_add(1, std::memory_order_relaxed); });
    jobs::wait_idle();
    MTT_EQ(counter.load(), 100);

    // submit from inside a worker chunk runs inline (nested submit policy)
    std::atomic<int> inline_counter{ 0 };
    jobs::parallel_for(16, 4, [&](u64, u64) {
        jobs::submit([&inline_counter] { inline_counter.fetch_add(1, std::memory_order_relaxed); });
    });
    MTT_EQ(inline_counter.load(), 4); // 4 chunks -> 4 inline submits, all done
}

MTT_TEST(jobs_wait_idle_is_quiet_when_empty) {
    jobs::wait_idle(); // must return immediately without blocking
    MTT_CHECK(true, "wait_idle on empty queue returns");
}
