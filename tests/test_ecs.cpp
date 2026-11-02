// Matted Engine - tests: ECS registry (generational entities, dense pools, views)
#include "test_framework.h"

#include "engine/core/ecs/registry.h"
#include "engine/core/math/math.h"

#include <set>
#include <typeindex>

using namespace mtt;
using namespace mtt::ecs;

struct Pos { vec3 v{ 0, 0, 0 }; };
struct Vel { vec3 v{ 0, 0, 0 }; };
struct Tag { int id = 0; };

MTT_TEST(entity_create_destroy_recycle) {
    Registry r;
    MTT_EQ(r.size(), size_t(0));

    Entity a = r.create();
    MTT_CHECK(a.valid(), "created entity is valid");
    MTT_CHECK(r.alive(a), "created entity is alive");
    MTT_EQ(r.size(), size_t(1));
    MTT_EQ(a.index(), 0u);
    MTT_EQ(a.version(), 1u);

    r.destroy(a);
    MTT_CHECK(!r.alive(a), "destroyed entity is not alive");
    MTT_EQ(r.size(), size_t(0));

    // recycle reuses the index but bumps the generation
    Entity b = r.create();
    MTT_EQ(b.index(), a.index());
    MTT_CHECK(b.version() == a.version() + 1, "generation bumped on recycle");
    MTT_CHECK(r.alive(b), "recycled handle alive");
    MTT_CHECK(!r.alive(a), "stale handle from before recycle is dead");
}

MTT_TEST(entity_many_and_free_list_order) {
    Registry r;
    Entity e0 = r.create(), e1 = r.create(), e2 = r.create();
    MTT_EQ(e0.index(), 0u);
    MTT_EQ(e1.index(), 1u);
    MTT_EQ(e2.index(), 2u);
    MTT_EQ(r.size(), size_t(3));

    r.destroy(e1);
    MTT_EQ(r.size(), size_t(2));
    Entity e3 = r.create();
    MTT_EQ(e3.index(), 1u);          // free list pops most recently destroyed
    MTT_EQ(r.size(), size_t(3));
    MTT_CHECK(r.alive(e3) && !r.alive(e1), "new handle alive, stale dead");
}

MTT_TEST(components_emplace_get_remove) {
    Registry r;
    Entity a = r.create(), b = r.create();

    Pos& pa = r.emplace<Pos>(a, Pos{ vec3(1, 2, 3) });
    MTT_EQ(pa.v.x, 1.0f);
    MTT_CHECK(r.has<Pos>(a), "has after emplace");
    MTT_CHECK(!r.has<Pos>(b), "other entity lacks component");

    const Pos* got = r.try_get<Pos>(a);
    MTT_CHECK(got != nullptr && got->v.y == 2.0f, "try_get returns stored value");
    MTT_CHECK(r.try_get<Pos>(b) == nullptr, "try_get missing returns null");

    // emplace over an existing component overwrites (no duplicate)
    r.emplace<Pos>(a, Pos{ vec3(9, 9, 9) });
    MTT_EQ(r.count_of(std::type_index(typeid(Pos))), size_t(1));
    MTT_NEAR(r.get<Pos>(a).v.z, 9.0f, 1e-6f);

    r.remove<Pos>(a);
    MTT_CHECK(!r.has<Pos>(a), "removed component gone");
    MTT_CHECK(r.try_get<Pos>(a) == nullptr, "try_get after remove returns null");
    r.remove<Pos>(a); // removing again is a no-op
    MTT_EQ(r.count_of(std::type_index(typeid(Pos))), size_t(0));
}

MTT_TEST(components_swap_remove_keeps_others) {
    Registry r;
    Entity a = r.create(), b = r.create(), c = r.create();
    r.emplace<Pos>(a, Pos{ vec3(1, 0, 0) });
    r.emplace<Pos>(b, Pos{ vec3(2, 0, 0) });
    r.emplace<Pos>(c, Pos{ vec3(3, 0, 0) });

    r.destroy(b); // middle element -> swap-remove moves c's data into b's slot

    MTT_CHECK(r.alive(a) && r.alive(c), "others survive");
    MTT_NEAR(r.get<Pos>(a).v.x, 1.0f, 1e-6f);
    MTT_NEAR(r.get<Pos>(c).v.x, 3.0f, 1e-6f);
    MTT_EQ(r.count_of(std::type_index(typeid(Pos))), size_t(2));

    // the removed entity's component must not alias into the recycled slot
    Entity d = r.create();
    MTT_CHECK(r.try_get<Pos>(d) == nullptr, "recycled entity starts without components");
}

MTT_TEST(destroy_entity_removes_components) {
    Registry r;
    Entity e = r.create();
    r.emplace<Pos>(e, Pos{ vec3(1, 1, 1) });
    r.emplace<Vel>(e, Vel{ vec3(2, 0, 0) });
    MTT_EQ(r.count_of(std::type_index(typeid(Pos))), size_t(1));
    MTT_EQ(r.count_of(std::type_index(typeid(Vel))), size_t(1));

    r.destroy(e);
    MTT_EQ(r.count_of(std::type_index(typeid(Pos))), size_t(0));
    MTT_EQ(r.count_of(std::type_index(typeid(Vel))), size_t(0));
}

MTT_TEST(view_two_components) {
    Registry r;
    Entity both0 = r.create(), both1 = r.create(), both2 = r.create();
    Entity only_pos = r.create(), only_vel = r.create();

    for (Entity e : { both0, both1, both2, only_pos }) r.emplace<Pos>(e, Pos{ vec3(1, 0, 0) });
    for (Entity e : { both0, both1, both2, only_vel }) r.emplace<Vel>(e, Vel{ vec3(1, 0, 0) });

    std::set<u32> seen;
    int calls = 0;
    r.view<Pos, Vel>([&](Entity e, Pos& p, Vel& v) {
        ++calls;
        seen.insert(e.index());
        p.v.x += v.v.x; // mutable access works
    });
    MTT_EQ(calls, 3);
    MTT_CHECK(seen.count(both0.index()) && seen.count(both1.index()) && seen.count(both2.index()),
              "view visits exactly the entities holding both components");
    MTT_NEAR(r.get<Pos>(both1).v.x, 2.0f, 1e-6f); // mutation applied
    MTT_NEAR(r.get<Pos>(only_pos).v.x, 1.0f, 1e-6f); // Pos-only entity untouched
}

MTT_TEST(each_iterates_dense_pool) {
    Registry r;
    for (int i = 0; i < 5; ++i) {
        Entity e = r.create();
        r.emplace<Pos>(e, Pos{ vec3(float(i), 0, 0) });
    }
    r.emplace<Tag>(r.create(), Tag{ 7 }); // noise: different component type

    float sum = 0;
    int visits = 0;
    r.each<Pos>([&](Entity, Pos& p) { sum += p.v.x; ++visits; });
    MTT_EQ(visits, 5);
    MTT_NEAR(sum, 10.0f, 1e-6f); // 0+1+2+3+4
}

MTT_TEST(registries_are_independent) {
    Registry r1, r2;
    Entity e = r1.create();
    r1.emplace<Pos>(e, Pos{ vec3(4, 4, 4) });
    MTT_EQ(r2.size(), size_t(0));
    MTT_CHECK(r2.try_get<Pos>(e) == nullptr, "no cross-registry bleed");
}
