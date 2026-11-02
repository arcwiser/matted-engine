// Matted Engine - physics: rigid bodies, collisions, destruction.
//
// Design notes (performance):
//  - Bodies live in one dense vector; BodyId is a stable handle via free-list.
//  - Broadphase: incremental sweep-and-prune on X (insertion sort exploits
//    temporal coherence), pairs only for awake bodies.
//  - Narrowphase: analytic sphere/box/plane + SAT box-box with face clipping.
//  - Solver: sequential impulses with warm starting, per-manifold persistence,
//    substepping, island sleeping. Contact state is reused across frames.
//  - Destruction: impulse-threshold grid fracture (2^3 split per level).
#pragma once
#include <functional>
#include <unordered_map>
#include <vector>
#include "engine/core/math/math.h"
#include "engine/core/mtt_types.h"

namespace mtt::physics {

using BodyId = u32;
constexpr BodyId kInvalidBody = 0xFFFFFFFFu;

enum class ShapeType : u8 { Sphere, Box, Plane };

struct Shape {
    ShapeType type = ShapeType::Sphere;
    float radius = 0.5f;          // Sphere
    vec3  half{0.5f, 0.5f, 0.5f}; // Box
    vec3  plane_n{0, 1, 0};       // Plane (body space): n . p + d = 0
    float plane_d = 0.0f;

    static Shape sphere(float r) { Shape s; s.type = ShapeType::Sphere; s.radius = r; return s; }
    static Shape box(vec3 half)  { Shape s; s.type = ShapeType::Box; s.half = half; return s; }
    static Shape plane(vec3 n, float d) {
        Shape s; s.type = ShapeType::Plane; s.plane_n = normalize(n); s.plane_d = d; return s;
    }
};

struct BodyMaterial {
    float friction = 0.6f;
    float restitution = 0.1f;
};

enum BodyFlags : u32 {
    kBodyStatic      = 1 << 0,
    kBodyAsleep      = 1 << 1,
    kBodyCanSleep    = 1 << 2,
    kBodyFracturable = 1 << 3,
};

struct Body {
    vec3 pos{0, 0, 0};
    quat rot = quat::identity();
    vec3 lin_vel{0, 0, 0};
    vec3 ang_vel{0, 0, 0};
    float inv_mass = 1.0f;
    vec3 inv_inertia{1, 1, 1};  // body-space diagonal
    Shape shape;
    BodyMaterial mat;
    u32 flags = kBodyCanSleep;
    u32 depth = 0;              // fracture depth
    u32 user = 0;               // external handle (e.g. ECS entity index)
    float sleep_timer = 0.0f;
    vec3 aabb_min{0, 0, 0}, aabb_max{0, 0, 0}; // world AABB, updated by world

    bool is_static() const { return (flags & kBodyStatic) != 0; }
    bool is_asleep() const { return (flags & kBodyAsleep) != 0; }
    float mass() const { return inv_mass > 0 ? 1.0f / inv_mass : 0.0f; }
};

struct RayHit {
    BodyId body = kInvalidBody;
    float dist = 0.0f;
    vec3 point{0, 0, 0};
    vec3 normal{0, 1, 0};
};

struct WorldConfig {
    vec3  gravity{0, -9.81f, 0};
    u32   substeps = 2;
    u32   velocity_iterations = 8;
    float baumgarte = 0.20f;
    float penetration_slop = 0.005f;
    float max_correction_vel = 2.0f;    // cap push-out speed (m/s)
    float rest_vel_threshold = 1.0f;    // restitution only above this approach speed
    float sleep_lin_tol = 0.08f;        // m/s
    float sleep_ang_tol = 0.25f;        // rad/s
    float sleep_time = 0.5f;            // seconds below tolerance before sleeping
    // destruction
    bool  fracture_enabled = true;
    float fracture_impulse_base = 130.0f;  // kg*m/s at depth 0
    float fracture_impulse_growth = 2.6f;  // threshold multiplier per depth
    u32   fracture_max_depth = 3;
    float fracture_min_size = 0.09f;       // smallest half-extent worth splitting
    u32   fracture_splits = 2;             // 2 -> 8 chunks per fracture
};

struct WorldStats {
    u32 bodies = 0;
    u32 awake = 0;
    u32 pairs = 0;
    u32 contacts = 0;
    u32 fractures = 0;
    float step_ms = 0.0f;
};

struct ContactPoint {
    vec3  pos{0, 0, 0};     // world position
    float pen = 0;          // penetration depth (>0 = overlapping)
    u32   feature = 0;      // persistent id for warm-start matching
    // solver scratch
    vec3  ra{0, 0, 0}, rb{0, 0, 0};
    vec3  t1{0, 0, 0}, t2{0, 0, 0};
    float mass_n = 0, mass_t1 = 0, mass_t2 = 0;
    float bias = 0, target_v = 0;
    float pn = 0, pt1 = 0, pt2 = 0;  // accumulated impulses
};

struct Manifold {
    BodyId a = kInvalidBody, b = kInvalidBody;
    u32 npts = 0;
    u32 stamp = 0;               // frame stamp for cache eviction
    vec3 normal{0, 1, 0};        // points from A to B
    ContactPoint pts[4];
    float friction = 0.6f, restitution = 0.1f;
};

struct FractureEvent {
    BodyId parent;
    vec3 impact;
    float impulse;
};

class World {
public:
    // callbacks fire during step(); keep them cheap
    std::function<void(BodyId, const Body&)> on_body_created;
    std::function<void(BodyId, const Body&)> on_body_removed;
    std::function<void(const FractureEvent&)> on_fracture;

    BodyId add_body(const Body& b);       // inertia derived from shape+mass
    void   remove_body(BodyId id);
    Body&       body(BodyId id)       { return bodies_[id]; }
    const Body& body(BodyId id) const { return bodies_[id]; }
    bool        valid(BodyId id) const { return id < bodies_.size() && id < free_flags_.size() && !free_flags_[id]; }
    size_t      body_count() const { return bodies_.size() - free_list_.size(); }

    WorldConfig&       config()       { return cfg_; }
    const WorldConfig& config() const { return cfg_; }
    WorldStats         stats() const  { return stats_; }

    void apply_impulse(BodyId id, vec3 impulse, vec3 world_point);
    void wake(BodyId id);

    // fixed-timestep simulation entry point
    void step(float dt);

    RayHit raycast(vec3 origin, vec3 dir, float max_dist) const;

    // manual destruction; impulse decides whether the threshold is met
    bool fracture(BodyId id, vec3 impact_point, float impulse);

private:
    friend struct SolverAccess;
    void update_aabb(Body& b);
    void broadphase();                     // SAP + pair list
    void narrowphase();                    // fills/updates manifolds_
    void integrate(float dt);              // forces + velocities
    void solve(float dt_sub);              // sequential impulses
    void integrate_positions(float dt);    // x += v dt
    void islands_and_sleep(float dt);
    void detect_fractures();               // scan contacts -> fracture queue
    void process_fractures();
    void destroy_body_internal(BodyId id);
    bool can_fracture(const Body& b, float impulse) const;

    WorldConfig cfg_;
    WorldStats stats_;
    std::vector<Body> bodies_;
    std::vector<u32> free_list_;
    std::vector<bool> free_flags_;         // parallel to bodies_
    std::vector<BodyId> active_;           // dynamic, awake bodies (dense)

    // broadphase
    struct SapEntry { float minx; u32 idx; };
    std::vector<SapEntry> sap_;
    std::vector<bool> in_sap_;             // parallel to bodies_: has a SAP entry
    std::vector<std::pair<u32, u32>> pairs_;
    u32 stamp_ = 0;

    // persistent contact cache: key = (minId<<32)|maxId
    std::unordered_map<u64, Manifold> manifolds_;
    std::vector<Manifold*> manifold_list_;

    // fracture queue (drained at end of step)
    struct FractureReq { BodyId body; vec3 point; float impulse; };
    std::vector<FractureReq> fracture_queue_;
    u32 fractures_this_frame_ = 0;
};

// implemented in narrowphase.cpp
void collide_pair(const Body& A, const Body& B, Manifold& m);

} // namespace mtt::physics
