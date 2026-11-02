// Matted Engine - job system: persistent worker pool with chunked parallel_for.
// - parallel_for(count, grain, fn) splits [0,count) into `grain`-sized chunks,
//   distributes them to workers and *also* runs chunks on the calling thread
//   (no idle main thread). Nested calls from workers run inline (safe fallback).
// - submit(fn) enqueues a fire-and-forget task; wait_idle() drains the queue.
#pragma once
#include <functional>
#include "engine/core/mtt_types.h"

namespace mtt::jobs {

void init(u32 worker_count = 0);   // 0 => hw threads - 1
void shutdown();
u32  thread_count();               // workers (excludes calling thread)
u32  worker_index();               // 0 = main thread, 1..N = workers

using Fn = std::function<void()>;
void submit(Fn fn);
void wait_idle();                  // block until all submitted tasks finish

// fn(begin, end) is called concurrently over chunks of `grain`.
void parallel_for(u64 count, u64 grain, const std::function<void(u64, u64)>& fn);

} // namespace mtt::jobs
