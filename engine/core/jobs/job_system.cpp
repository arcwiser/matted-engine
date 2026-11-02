#include "engine/core/jobs/job_system.h"
#include "engine/core/log.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace mtt::jobs {

namespace {

struct Pool {
    std::mutex mu;
    std::condition_variable cv;
    std::deque<Fn> queue;
    i32 active = 0;          // tasks currently executing
    bool stop = false;
};
Pool* g_pool = nullptr;
std::vector<std::thread> g_threads;
thread_local bool t_is_worker = false;
thread_local u32 t_worker_index = 0; // main thread = 0, workers = 1..N

void worker_loop(Pool* pool, u32 index) {
    t_is_worker = true;
    t_worker_index = index;
    for (;;) {
        Fn task;
        {
            std::unique_lock<std::mutex> lk(pool->mu);
            pool->cv.wait(lk, [&] { return pool->stop || !pool->queue.empty(); });
            if (pool->stop && pool->queue.empty()) return;
            task = std::move(pool->queue.front());
            pool->queue.pop_front();
            ++pool->active;
        }
        task();
        {
            std::lock_guard<std::mutex> lk(pool->mu);
            --pool->active;
            if (pool->queue.empty() && pool->active == 0) pool->cv.notify_all();
        }
    }
}

struct Chunker {
    const std::function<void(u64, u64)>* fn;
    std::atomic<u64> next{0};
    std::atomic<u32> running{0};
    u64 count, grain;
    Pool* pool;

    void run_chunks() {
        for (;;) {
            const u64 c = next.fetch_add(1, std::memory_order_relaxed);
            const u64 begin = c * grain;
            if (begin >= count) break;
            const u64 end = begin + grain > count ? count : begin + grain;
            (*fn)(begin, end);
        }
        // completion bookkeeping
        std::lock_guard<std::mutex> lk(pool->mu);
        if (--running == 0) pool->cv.notify_all();
    }
};

} // namespace

void init(u32 worker_count) {
    if (g_pool) return;
    if (worker_count == 0) worker_count = std::thread::hardware_concurrency();
    if (worker_count > 2) worker_count -= 1; // leave one core for the main thread
    if (worker_count < 1) worker_count = 1;

    g_pool = new Pool();
    g_threads.reserve(worker_count);
    for (u32 i = 0; i < worker_count; ++i)
        g_threads.emplace_back(worker_loop, g_pool, i + 1);
    log::info("job system: %u worker threads", worker_count);
}

void shutdown() {
    if (!g_pool) return;
    {
        std::lock_guard<std::mutex> lk(g_pool->mu);
        g_pool->stop = true;
    }
    g_pool->cv.notify_all();
    for (auto& t : g_threads) t.join();
    g_threads.clear();
    delete g_pool;
    g_pool = nullptr;
}

u32 thread_count() { return g_pool ? (u32)g_threads.size() : 0; }
u32 worker_index() { return t_worker_index; }

void submit(Fn fn) {
    if (!g_pool) { fn(); return; } // no pool: run inline
    if (t_is_worker) { fn(); return; } // no nested submit from workers
    {
        std::lock_guard<std::mutex> lk(g_pool->mu);
        g_pool->queue.push_back(std::move(fn));
    }
    g_pool->cv.notify_one();
}

void wait_idle() {
    if (!g_pool) return;
    std::unique_lock<std::mutex> lk(g_pool->mu);
    g_pool->cv.wait(lk, [] { return g_pool->queue.empty() && g_pool->active == 0; });
}

void parallel_for(u64 count, u64 grain, const std::function<void(u64, u64)>& fn) {
    if (count == 0) return;
    if (grain == 0) grain = 1;
    const u64 chunks = (count + grain - 1) / grain;

    if (!g_pool || t_is_worker) { // inline fallback (also covers nested calls)
        for (u64 c = 0; c < chunks; ++c) {
            const u64 begin = c * grain;
            const u64 end = begin + grain > count ? count : begin + grain;
            fn(begin, end);
        }
        return;
    }

    Chunker chunker{ &fn, 0, 0, count, grain, g_pool };

    // one wrapper task per worker (they loop over chunks internally)
    const u32 n_workers = (u32)g_threads.size();
    const u32 n_tasks = chunks < n_workers ? (u32)chunks : n_workers;
    chunker.running.store(n_tasks, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(g_pool->mu);
        for (u32 i = 0; i < n_tasks; ++i) {
            Chunker* cp = &chunker;
            g_pool->queue.push_back([cp] { cp->run_chunks(); });
        }
    }
    g_pool->cv.notify_all();

    // main thread helps instead of idling
    chunker.run_chunks();

    // wait for the rest
    std::unique_lock<std::mutex> lk(g_pool->mu);
    g_pool->cv.wait(lk, [&] { return chunker.running.load() == 0; });
}

} // namespace mtt::jobs
