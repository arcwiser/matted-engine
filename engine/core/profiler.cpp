#include "engine/core/profiler.h"
#include "engine/core/log.h"
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <vector>
#include <algorithm>

namespace mtt::prof {

namespace {
using Clock = std::chrono::steady_clock;

struct ZoneStats {
    const char* name = nullptr;
    std::atomic<i64> frame_ns{0};   // accumulated in current frame
    std::atomic<i64> total_ns{0};   // folded history (avg over frames)
    std::atomic<u64> frame_count{0};
    std::atomic<u64> total_count{0};
};

constexpr int kMaxZones = 256;
ZoneStats g_zones[kMaxZones];
std::atomic<int> g_zone_next{0};
std::mutex g_zone_mutex; // only used during registration

thread_local i64 g_child_ns = 0; // simple self-time accounting (subtract children)
} // namespace

u32 zone_id(const char* name) {
    std::lock_guard<std::mutex> lk(g_zone_mutex);
    const int n = g_zone_next.load(std::memory_order_relaxed);
    for (int i = 0; i < n; ++i)
        if (std::strcmp(g_zones[i].name, name) == 0) return (u32)i;
    const int id = n;
    g_zones[id].name = name;
    g_zone_next.store(n + 1, std::memory_order_relaxed);
    return (u32)id;
}

ScopedZone::ScopedZone(u32 zone_id_) : id_(zone_id_), t0_(Clock::now().time_since_epoch().count()) {}
ScopedZone::~ScopedZone() {
    const i64 t1 = Clock::now().time_since_epoch().count();
    const i64 self = (t1 - t0_) - g_child_ns; // rough self time
    ZoneStats& z = g_zones[id_];
    z.frame_ns.fetch_add(self, std::memory_order_relaxed);
    z.frame_count.fetch_add(1, std::memory_order_relaxed);
    g_child_ns = 0; // nested zones report separately
}

void frame_begin() {
    const int n = g_zone_next.load(std::memory_order_relaxed);
    for (int i = 0; i < n; ++i) {
        g_zones[i].frame_ns.store(0, std::memory_order_relaxed);
        g_zones[i].frame_count.store(0, std::memory_order_relaxed);
    }
}

void frame_end() {
    const int n = g_zone_next.load(std::memory_order_relaxed);
    for (int i = 0; i < n; ++i) {
        g_zones[i].total_ns.fetch_add(g_zones[i].frame_ns.load(std::memory_order_relaxed));
        g_zones[i].total_count.fetch_add(g_zones[i].frame_count.load(std::memory_order_relaxed));
    }
}

double zone_ms(u32 id) {
    return (double)g_zones[id].frame_ns.load(std::memory_order_relaxed) / 1e6;
}
u64 zone_count(u32 id) { return g_zones[id].frame_count.load(std::memory_order_relaxed); }

void report_top(int n, double* out_ms) {
    const int count = g_zone_next.load(std::memory_order_relaxed);
    u32 order[kMaxZones];
    for (int i = 0; i < count; ++i) order[i] = (u32)i;
    std::sort(order, order + count, [](u32 a, u32 b) {
        return g_zones[a].frame_ns.load() > g_zones[b].frame_ns.load();
    });
    double top_total = 0;
    for (int i = 0; i < count && i < n; ++i) {
        const ZoneStats& z = g_zones[order[i]];
        const double ms = (double)z.frame_ns.load() / 1e6;
        top_total += ms;
        log::info("  prof %-28s %8.3f ms  (%llu calls)",
                  z.name, ms, (unsigned long long)z.frame_count.load());
    }
    if (out_ms) *out_ms = top_total;
}

} // namespace mtt::prof
