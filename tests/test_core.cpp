// Matted Engine - tests: entity bit packing, profiler, logging
#include "test_framework.h"

#include "engine/core/log.h"
#include "engine/core/mtt_types.h"
#include "engine/core/profiler.h"

#include <chrono>

using namespace mtt;

MTT_TEST(entity_bit_packing) {
    // layout: 20-bit index | 12-bit version
    Entity e;
    e.id = 5u | (7u << 20);
    MTT_EQ(e.index(), 5u);
    MTT_EQ(e.version(), 7u);
    MTT_CHECK(e.valid(), "assembled entity is valid");
    MTT_CHECK(e == Entity{ 5u | (7u << 20) }, "equality by id");

    const Entity stale{ 5u | (8u << 20) }; // same index, bumped version
    MTT_CHECK(e != stale, "version bump changes identity");

    Entity invalid;
    MTT_CHECK(!invalid.valid(), "default entity is invalid");

    // max encodable index/version round-trip
    Entity mx;
    mx.id = 0xFFFFFu | (0xFFFu << 20);
    MTT_EQ(mx.index(), 0xFFFFFu);
    MTT_EQ(mx.version(), 0xFFFu);
}

MTT_TEST(profiler_zone_accounting) {
    const u32 a = prof::zone_id("test_zone_a");
    const u32 a2 = prof::zone_id("test_zone_a");
    const u32 b = prof::zone_id("test_zone_b");
    MTT_EQ(a, a2);                       // interning is stable
    MTT_CHECK(a != b, "distinct names get distinct ids");

    prof::frame_begin();
    {
        const u32 busy = prof::zone_id("test_zone_busy");
        prof::ScopedZone z(busy);
        // spin ~2ms so the zone measures > 0
        volatile double sink = 0.0;
        const auto t0 = std::chrono::steady_clock::now();
        while (std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() < 2.0)
            sink += 1.0;
    }
    const u32 busy = prof::zone_id("test_zone_busy");
    MTT_CHECK(prof::zone_ms(busy) > 0.0, "zone accumulated time this frame");
    MTT_CHECK(prof::zone_count(busy) >= 1, "zone hit counted");

    prof::frame_end(); // folds into history
    prof::frame_begin();
    MTT_NEAR(prof::zone_ms(busy), 0.0, 1e-9); // per-frame accumulation reset
    MTT_EQ(prof::zone_count(busy), 0u);
    prof::frame_end();

    // report_top must not crash and reports the requested count cap
    prof::report_top(4);
}

MTT_TEST(log_levels_no_crash) {
    // coverage: every level + formatted args must go through the writer
    const log::Level levels[] = { log::Level::Debug, log::Level::Info, log::Level::Warn, log::Level::Error };
    for (log::Level lvl : levels) {
        log::set_level(lvl);
        log::write(lvl, "test fmt %d %s %.2f", 42, "str", 3.5);
        log::debug("debug line");
        log::info("info line");
        log::warn("warn line");
        log::error("error line");
    }
    log::set_level(log::Level::Info); // leave a sane default
}
