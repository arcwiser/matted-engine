// Matted Engine - physics world: broadphase, solver, islands, destruction.
#include "engine/physics/physics.h"
#include "engine/core/jobs/job_system.h"
#include "engine/core/profiler.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <random>

namespace mtt::physics {

// ---------------------------------------------------------------- helpers
static inline i64 now_ns() {
    return (i64)std::chrono::steady_clock::now().time_since_epoch().count();
}

// world-space application of body-space diagonal inverse inertia
static inline vec3 apply_inv_inertia(const Body& b, const vec3& v) {
    return b.rot.rotate(b.inv_inertia * b.rot.rotate_inv(v));
}

static void compute_mass_data(Body& b) {
    if (b.inv_mass <= 0.0f || (b.flags & kBodyStatic)) {
        b.inv_mass = 0.0f;
        b.inv_inertia = vec3(0);
        return;
    }
    const float m = 1.0f / b.inv_mass;
    switch (b.shape.type) {
        case ShapeType::Sphere: {
            const float r = b.shape.radius;
            const float i = 0.4f * m * r * r;
            b.inv_inertia = vec3(i > 1e-12f ? 1.0f / i : 0.0f);
        } break;
        case ShapeType::Box: {
            const vec3 full = b.shape.half * 2.0f;
            const float ix = m / 12.0f * (full.y * full.y + full.z * full.z);
            const float iy = m / 12.0f * (full.x * full.x + full.z * full.z);
            const float iz = m / 12.0f * (full.x * full.x + full.y * full.y);
            b.inv_inertia = vec3(ix > 1e-12f ? 1.0f / ix : 0.0f,
                                 iy > 1e-12f ? 1.0f / iy : 0.0f,
                                 iz > 1e-12f ? 1.0f / iz : 0.0f);
        } break;
        case ShapeType::Plane:
            b.inv_inertia = vec3(0);
        break;
    }
}

static void update_body_aabb(Body& b) {
    switch (b.shape.type) {
        case ShapeType::Sphere:
            b.aabb_min = b.pos - vec3(b.shape.radius);
            b.aabb_max = b.pos + vec3(b.shape.radius);
            break;
        case ShapeType::Box: {
            float R[9];
            quat_to_mat3(b.rot, R);
            const vec3& h = b.shape.half;
            const float ex = std::fabs(R[0]) * h.x + std::fabs(R[3]) * h.y + std::fabs(R[6]) * h.z;
            const float ey = std::fabs(R[1]) * h.x + std::fabs(R[4]) * h.y + std::fabs(R[7]) * h.z;
            const float ez = std::fabs(R[2]) * h.x + std::fabs(R[5]) * h.y + std::fabs(R[8]) * h.z;
            b.aabb_min = b.pos - vec3(ex, ey, ez);
            b.aabb_max = b.pos + vec3(ex, ey, ez);
        } break;
        case ShapeType::Plane:
            b.aabb_min = vec3(-1e9f);
            b.aabb_max = vec3(1e9f);
            break;
    }
}

// ---------------------------------------------------------------- bodies
BodyId World::add_body(const Body& b_in) {
    Body b = b_in;
    if (b.flags & kBodyStatic) b.inv_mass = 0.0f;
    compute_mass_data(b);
    update_body_aabb(b);

    BodyId id;
    if (!free_list_.empty()) {
        id = free_list_.back();
        free_list_.pop_back();
        free_flags_[id] = false;
        in_sap_[id] = false;
        bodies_[id] = b;
    } else {
        id = (BodyId)bodies_.size();
        bodies_.push_back(b);
        free_flags_.push_back(false);
        in_sap_.push_back(false);
    }
    if (on_body_created) on_body_created(id, bodies_[id]);
    return id;
}

void World::destroy_body_internal(BodyId id) {
    if (id >= bodies_.size() || free_flags_[id]) return;
    if (on_body_removed) on_body_removed(id, bodies_[id]);
    free_flags_[id] = true;
    in_sap_[id] = false;
    free_list_.push_back(id);
    for (auto it = manifolds_.begin(); it != manifolds_.end();) {
        const Manifold& m = it->second;
        if (m.a == id || m.b == id) it = manifolds_.erase(it);
        else ++it;
    }
}

void World::remove_body(BodyId id) { destroy_body_internal(id); }

void World::wake(BodyId id) {
    if (id >= bodies_.size() || free_flags_[id]) return;
    Body& b = bodies_[id];
    if (b.is_static()) return;
    b.flags &= ~kBodyAsleep;
    b.sleep_timer = 0.0f;
}

void World::apply_impulse(BodyId id, vec3 impulse, vec3 world_point) {
    if (id >= bodies_.size() || free_flags_[id]) return;
    Body& b = bodies_[id];
    if (b.inv_mass <= 0.0f) return;
    wake(id);
    b.lin_vel += impulse * b.inv_mass;
    b.ang_vel += apply_inv_inertia(b, cross(world_point - b.pos, impulse));
}

// ---------------------------------------------------------------- broadphase
void World::broadphase() {
    MTT_PROFILE_ZONE("phys.broadphase");
    const size_t n = bodies_.size();

    // refresh AABBs (parallel; vector<bool> reads are safe concurrently)
    jobs::parallel_for(n, 256, [&](u64 begin, u64 end) {
        for (u64 i = begin; i < end; ++i)
            if (!free_flags_[i]) update_body_aabb(bodies_[i]);
    });

    // compact dead entries + refresh minx
    size_t w = 0;
    for (size_t i = 0; i < sap_.size(); ++i) {
        const u32 idx = sap_[i].idx;
        if (idx < n && !free_flags_[idx]) {
            sap_[w].minx = bodies_[idx].aabb_min.x;
            sap_[w].idx = idx;
            ++w;
        }
    }
    sap_.resize(w);

    // append bodies not yet tracked
    for (size_t i = 0; i < n; ++i) {
        if (!free_flags_[i] && !in_sap_[i]) {
            sap_.push_back({ bodies_[i].aabb_min.x, (u32)i });
            in_sap_[i] = true;
        }
    }

    // insertion sort (temporal coherence makes this near-linear)
    for (size_t i = 1; i < sap_.size(); ++i) {
        SapEntry key = sap_[i];
        size_t j = i;
        while (j > 0 && sap_[j - 1].minx > key.minx) { sap_[j] = sap_[j - 1]; --j; }
        sap_[j] = key;
    }

    // sweep for pairs
    pairs_.clear();
    for (size_t i = 0; i < sap_.size(); ++i) {
        const u32 ai = sap_[i].idx;
        const Body& A = bodies_[ai];
        const float amaxx = A.aabb_max.x;
        for (size_t j = i + 1; j < sap_.size(); ++j) {
            const u32 bi = sap_[j].idx;
            if (sap_[j].minx > amaxx) break;
            const Body& B = bodies_[bi];
            if (A.is_static() && B.is_static()) continue;
            if (A.is_asleep() && B.is_asleep()) continue;
            if (A.aabb_min.y > B.aabb_max.y || A.aabb_max.y < B.aabb_min.y) continue;
            if (A.aabb_min.z > B.aabb_max.z || A.aabb_max.z < B.aabb_min.z) continue;
            pairs_.push_back({ ai, bi });
        }
    }

    // a wake body touching a sleeping one wakes it
    for (const auto& pr : pairs_) {
        Body& A = bodies_[pr.first];
        Body& B = bodies_[pr.second];
        if (A.is_asleep() && !B.is_asleep() && !B.is_static()) wake(pr.first);
        else if (B.is_asleep() && !A.is_asleep() && !A.is_static()) wake(pr.second);
    }
    stats_.pairs = (u32)pairs_.size();
}

// ---------------------------------------------------------------- narrowphase
static inline u64 pair_key(u32 a, u32 b) {
    return a < b ? ((u64)a << 32) | b : ((u64)b << 32) | a;
}

void World::narrowphase() {
    MTT_PROFILE_ZONE("phys.narrowphase");
    ++stamp_;
    manifold_list_.clear();
    u32 total_contacts = 0;

    for (const auto& pr : pairs_) {
        Body& A = bodies_[pr.first];
        Body& B = bodies_[pr.second];
        const u64 key = pair_key(pr.first, pr.second);
        Manifold old;
        const auto it = manifolds_.find(key);
        if (it != manifolds_.end()) old = it->second;

        Manifold m;
        m.a = pr.first;
        m.b = pr.second;
        m.stamp = stamp_;
        collide_pair(A, B, m);
        if (m.npts == 0) { manifolds_.erase(key); continue; }

        // warm-start matching by feature id
        if (old.npts > 0) {
            for (u32 i = 0; i < m.npts; ++i)
                for (u32 j = 0; j < old.npts; ++j)
                    if (old.pts[j].feature == m.pts[i].feature) {
                        m.pts[i].pn  = old.pts[j].pn;
                        m.pts[i].pt1 = old.pts[j].pt1;
                        m.pts[i].pt2 = old.pts[j].pt2;
                        break;
                    }
        }
        total_contacts += m.npts;
        auto res = manifolds_.insert_or_assign(key, m);
        manifold_list_.push_back(&res.first->second);
    }

    // evict stale entries
    for (auto it = manifolds_.begin(); it != manifolds_.end();) {
        if (it->second.stamp != stamp_) it = manifolds_.erase(it);
        else ++it;
    }
    stats_.contacts = total_contacts;
}

// ---------------------------------------------------------------- solver
static inline float eff_mass(const Body& A, const Body& B,
                             const vec3& ra, const vec3& rb, const vec3& dir) {
    const vec3 cra = cross(ra, dir);
    const vec3 crb = cross(rb, dir);
    const float kn = A.inv_mass + B.inv_mass
                   + dot(apply_inv_inertia(A, cra), cra)
                   + dot(apply_inv_inertia(B, crb), crb);
    return kn > 1e-12f ? 1.0f / kn : 0.0f;
}

static inline vec3 rel_vel(const Body& A, const Body& B, const vec3& ra, const vec3& rb) {
    return (B.lin_vel + cross(B.ang_vel, rb)) - (A.lin_vel + cross(A.ang_vel, ra));
}

static inline void apply_contact(Body& A, Body& B, const vec3& imp,
                                 const vec3& ra, const vec3& rb) {
    if (A.inv_mass > 0.0f) {
        A.lin_vel -= imp * A.inv_mass;
        A.ang_vel -= apply_inv_inertia(A, cross(ra, imp));
    }
    if (B.inv_mass > 0.0f) {
        B.lin_vel += imp * B.inv_mass;
        B.ang_vel += apply_inv_inertia(B, cross(rb, imp));
    }
}

void World::solve(float dt_sub) {
    MTT_PROFILE_ZONE("phys.solve");
    const bool warm = true;
    static thread_local std::vector<Manifold*> active_ms;
    active_ms.clear();
    for (Manifold* m : manifold_list_)
        if (!(bodies_[m->a].is_asleep() && bodies_[m->b].is_asleep()))
            active_ms.push_back(m);

    // prepare
    for (Manifold* m : active_ms) {
        Body& A = bodies_[m->a];
        Body& B = bodies_[m->b];
        const vec3 n = m->normal;
        for (u32 i = 0; i < m->npts; ++i) {
            ContactPoint& p = m->pts[i];
            p.ra = p.pos - A.pos;
            p.rb = p.pos - B.pos;
            vec3 t1 = (std::fabs(n.y) < 0.9f) ? cross(n, vec3(0, 1, 0)) : cross(n, vec3(1, 0, 0));
            t1 = normalize(t1);
            p.t1 = t1;
            p.t2 = cross(n, t1);
            p.mass_n  = eff_mass(A, B, p.ra, p.rb, n);
            p.mass_t1 = eff_mass(A, B, p.ra, p.rb, p.t1);
            p.mass_t2 = eff_mass(A, B, p.ra, p.rb, p.t2);
            const float vn = dot(rel_vel(A, B, p.ra, p.rb), n);
            p.target_v = vn < -cfg_.rest_vel_threshold ? -m->restitution * vn : 0.0f;
            const float bias = cfg_.baumgarte / dt_sub * maxf(0.0f, p.pen - cfg_.penetration_slop);
            p.bias = minf(bias, cfg_.max_correction_vel);
            if (warm && (p.pn != 0.0f || p.pt1 != 0.0f || p.pt2 != 0.0f))
                apply_contact(A, B, n * p.pn + p.t1 * p.pt1 + p.t2 * p.pt2, p.ra, p.rb);
        }
    }

    // iterate
    for (u32 it = 0; it < cfg_.velocity_iterations; ++it) {
        for (Manifold* m : active_ms) {
            Body& A = bodies_[m->a];
            Body& B = bodies_[m->b];
            const vec3 n = m->normal;
            const float mu = m->friction;
            for (u32 i = 0; i < m->npts; ++i) {
                ContactPoint& p = m->pts[i];
                { // normal
                    const float vn = dot(rel_vel(A, B, p.ra, p.rb), n);
                    const float dlambda = p.mass_n * ((p.bias + p.target_v) - vn);
                    const float pn_new = maxf(p.pn + dlambda, 0.0f);
                    apply_contact(A, B, n * (pn_new - p.pn), p.ra, p.rb);
                    p.pn = pn_new;
                }
                { // friction t1
                    const float vt = dot(rel_vel(A, B, p.ra, p.rb), p.t1);
                    const float max_f = mu * p.pn;
                    const float pt_new = clampf(p.pt1 - vt * p.mass_t1, -max_f, max_f);
                    apply_contact(A, B, p.t1 * (pt_new - p.pt1), p.ra, p.rb);
                    p.pt1 = pt_new;
                }
                { // friction t2
                    const float vt = dot(rel_vel(A, B, p.ra, p.rb), p.t2);
                    const float max_f = mu * p.pn;
                    const float pt_new = clampf(p.pt2 - vt * p.mass_t2, -max_f, max_f);
                    apply_contact(A, B, p.t2 * (pt_new - p.pt2), p.ra, p.rb);
                    p.pt2 = pt_new;
                }
            }
        }
    }
}

// ---------------------------------------------------------------- integration
void World::integrate(float dt) {
    for (size_t i = 0; i < bodies_.size(); ++i) {
        if (free_flags_[i]) continue;
        Body& b = bodies_[i];
        if (b.inv_mass <= 0.0f || b.is_asleep()) continue;
        b.lin_vel += cfg_.gravity * dt;
    }
}

void World::integrate_positions(float dt) {
    for (size_t i = 0; i < bodies_.size(); ++i) {
        if (free_flags_[i]) continue;
        Body& b = bodies_[i];
        if (b.inv_mass <= 0.0f || b.is_asleep()) continue;
        b.pos += b.lin_vel * dt;
        const quat wq(b.ang_vel.x, b.ang_vel.y, b.ang_vel.z, 0.0f);
        const quat dq = wq * b.rot;
        b.rot = quat(b.rot.x + 0.5f * dq.x * dt,
                     b.rot.y + 0.5f * dq.y * dt,
                     b.rot.z + 0.5f * dq.z * dt,
                     b.rot.w + 0.5f * dq.w * dt).normalized();
    }
}

// ---------------------------------------------------------------- islands & sleep
void World::islands_and_sleep(float dt) {
    MTT_PROFILE_ZONE("phys.sleep");
    const u32 n = (u32)bodies_.size();
    if (n == 0) return;

    std::vector<u32> parent(n);
    for (u32 i = 0; i < n; ++i) parent[i] = i;
    auto find = [&parent](u32 x) {
        while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; }
        return x;
    };
    for (const Manifold* m : manifold_list_) {
        if (bodies_[m->a].inv_mass > 0.0f && bodies_[m->b].inv_mass > 0.0f) {
            const u32 ra = find(m->a), rb = find(m->b);
            if (ra != rb) parent[ra] = rb;
        }
    }

    std::vector<u8> set_awake(n, 0);
    for (u32 i = 0; i < n; ++i) {
        if (free_flags_[i]) continue;
        const Body& b = bodies_[i];
        if (b.inv_mass <= 0.0f || !(b.flags & kBodyCanSleep)) {
            set_awake[find(i)] = 1; // statics / non-sleepable anchor their island awake
            continue;
        }
        const float lin = length(b.lin_vel), ang = length(b.ang_vel);
        if (lin > cfg_.sleep_lin_tol || ang > cfg_.sleep_ang_tol) set_awake[find(i)] = 1;
    }

    u32 awake_count = 0;
    for (u32 i = 0; i < n; ++i) {
        if (free_flags_[i]) continue;
        Body& b = bodies_[i];
        if (b.inv_mass <= 0.0f || !(b.flags & kBodyCanSleep)) continue;
        if (set_awake[find(i)]) {
            b.sleep_timer = 0.0f;
            b.flags &= ~kBodyAsleep;
            ++awake_count;
        } else {
            b.sleep_timer += dt;
            if (b.sleep_timer >= cfg_.sleep_time) {
                b.flags |= kBodyAsleep;
                b.lin_vel = vec3(0);
                b.ang_vel = vec3(0);
            } else {
                ++awake_count;
            }
        }
    }
    stats_.awake = awake_count;
}

// ---------------------------------------------------------------- destruction
bool World::can_fracture(const Body& b, float impulse) const {
    if (!cfg_.fracture_enabled) return false;
    if (!(b.flags & kBodyFracturable)) return false;
    if (b.shape.type != ShapeType::Box) return false;
    if (b.depth >= cfg_.fracture_max_depth) return false;
    const vec3& h = b.shape.half;
    const float min_half = minf(h.x, minf(h.y, h.z));
    if (min_half < cfg_.fracture_min_size * (float)cfg_.fracture_splits) return false;
    const float threshold = cfg_.fracture_impulse_base *
                            std::pow(cfg_.fracture_impulse_growth, (float)b.depth);
    return impulse >= threshold;
}

bool World::fracture(BodyId id, vec3 impact_point, float impulse) {
    if (id >= bodies_.size() || free_flags_[id]) return false;
    const float want = cfg_.fracture_impulse_base *
                       std::pow(cfg_.fracture_impulse_growth, (float)bodies_[id].depth);
    if (!can_fracture(bodies_[id], maxf(impulse, want))) return false;
    fracture_queue_.push_back({ id, impact_point, maxf(impulse, want) });
    return true;
}

void World::detect_fractures() {
    if (!cfg_.fracture_enabled) return;
    for (const Manifold* m : manifold_list_) {
        for (u32 i = 0; i < m->npts; ++i) {
            const float impulse = m->pts[i].pn;
            if (impulse < cfg_.fracture_impulse_base) continue; // cheap pre-filter
            if (can_fracture(bodies_[m->a], impulse))
                fracture_queue_.push_back({ m->a, m->pts[i].pos, impulse });
            if (can_fracture(bodies_[m->b], impulse))
                fracture_queue_.push_back({ m->b, m->pts[i].pos, impulse });
        }
    }
}

void World::process_fractures() {
    if (fracture_queue_.empty()) { fracture_queue_.clear(); return; }
    MTT_PROFILE_ZONE("phys.fracture");
    std::mt19937 rng(0x4d4154u); // deterministic
    auto rand01 = [&rng] { return (float)(rng() % 10000) / 10000.0f; };

    std::vector<BodyId> seen;
    for (const FractureReq& req : fracture_queue_) {
        if (std::find(seen.begin(), seen.end(), req.body) != seen.end()) continue;
        if (req.body >= bodies_.size() || free_flags_[req.body]) continue;
        seen.push_back(req.body);

        const Body parent = bodies_[req.body]; // copy: we destroy the original
        const u32 k = maxu(2, cfg_.fracture_splits);
        const vec3 cell = parent.shape.half * (2.0f / (float)k);

        for (u32 iz = 0; iz < k; ++iz)
            for (u32 iy = 0; iy < k; ++iy)
                for (u32 ix = 0; ix < k; ++ix) {
                    Body child;
                    child.shape = Shape::box(cell * (0.5f * (0.82f + 0.15f * rand01())));
                    const vec3 jitter((rand01() - 0.5f), (rand01() - 0.5f), (rand01() - 0.5f));
                    const vec3 local = vec3(-(float)k + 1.0f + 2.0f * ix,
                                            -(float)k + 1.0f + 2.0f * iy,
                                            -(float)k + 1.0f + 2.0f * iz) * (cell * 0.5f)
                                       + jitter * cell * 0.18f;
                    child.pos = parent.pos + parent.rot.rotate(local);
                    child.rot = parent.rot;
                    // inherit velocity + radial impulse away from the impact
                    const vec3 dir = child.pos - req.point;
                    const float dl = length(dir);
                    const vec3 radial = dl > 1e-4f ? dir / dl : vec3(0, 1, 0);
                    const float kick = req.impulse / maxf(parent.mass(), 1.0f) * 0.35f;
                    child.lin_vel = parent.lin_vel + radial * kick + vec3(0, 0.35f, 0);
                    child.ang_vel = vec3((rand01() - 0.5f) * 3.0f,
                                         (rand01() - 0.5f) * 3.0f,
                                         (rand01() - 0.5f) * 3.0f);
                    child.mat = parent.mat;
                    child.depth = parent.depth + 1;
                    child.user = parent.user;
                    child.flags = kBodyCanSleep;
                    if (child.depth < cfg_.fracture_max_depth) child.flags |= kBodyFracturable;
                    add_body(child); // fires on_body_created
                }

        destroy_body_internal(req.body);
        if (on_fracture) on_fracture({ req.body, req.point, req.impulse });
        ++fractures_this_frame_;
    }
    fracture_queue_.clear();
    stats_.fractures += fractures_this_frame_;
}

// ---------------------------------------------------------------- raycast
RayHit World::raycast(vec3 origin, vec3 dir, float max_dist) const {
    RayHit best;
    dir = normalize(dir);
    for (size_t i = 0; i < bodies_.size(); ++i) {
        if (free_flags_[i]) continue;
        const Body& b = bodies_[i];
        float t = -1.0f;
        vec3 n(0, 1, 0);

        switch (b.shape.type) {
            case ShapeType::Sphere: {
                const vec3 oc = origin - b.pos;
                const float bq = dot(oc, dir);
                const float cq = dot(oc, oc) - b.shape.radius * b.shape.radius;
                const float disc = bq * bq - cq;
                if (disc < 0) continue;
                t = -bq - std::sqrt(disc);
                if (t < 0) t = -bq + std::sqrt(disc);
                if (t < 0) continue;
                n = normalize(origin + dir * t - b.pos);
            } break;
            case ShapeType::Box: {
                const quat inv = b.rot.conjugate();
                const vec3 o = inv.rotate(origin - b.pos);
                const vec3 d = inv.rotate(dir);
                const float h[3]  = { b.shape.half.x, b.shape.half.y, b.shape.half.z };
                const float oo[3] = { o.x, o.y, o.z };
                const float dd[3] = { d.x, d.y, d.z };
                float tmin = -1e30f, tmax = 1e30f;
                int axis = 0;
                float sign = -1.0f;
                bool miss = false;
                for (int a = 0; a < 3; ++a) {
                    if (std::fabs(dd[a]) < 1e-9f) {
                        if (oo[a] < -h[a] || oo[a] > h[a]) { miss = true; break; }
                        continue;
                    }
                    float t1 = (-h[a] - oo[a]) / dd[a];
                    float t2 = ( h[a] - oo[a]) / dd[a];
                    float s = -1.0f; // entering the -face when dd > 0
                    if (t1 > t2) { std::swap(t1, t2); s = 1.0f; }
                    if (t1 > tmin) { tmin = t1; axis = a; sign = s; }
                    tmax = minf(tmax, t2);
                    if (tmin > tmax) { miss = true; break; }
                }
                if (miss || tmin > max_dist || tmin < 0.0f) continue;
                t = tmin;
                const vec3 ln(axis == 0 ? sign : 0.0f,
                              axis == 1 ? sign : 0.0f,
                              axis == 2 ? sign : 0.0f);
                n = b.rot.rotate(ln);
            } break;
            case ShapeType::Plane: {
                vec3 pn; float pd;
                pn = b.rot.rotate(b.shape.plane_n);
                pd = b.shape.plane_d - dot(pn, b.pos);
                const float denom = dot(pn, dir);
                if (std::fabs(denom) < 1e-9f) continue;
                t = -(dot(pn, origin) + pd) / denom;
                if (t < 0 || t > max_dist) continue;
                n = pn;
            } break;
        }
        if (t >= 0.0f && t <= max_dist && (best.body == kInvalidBody || t < best.dist)) {
            best.body = (BodyId)i;
            best.dist = t;
            best.point = origin + dir * t;
            best.normal = n;
        }
    }
    return best;
}

// ---------------------------------------------------------------- step
void World::step(float dt) {
    MTT_PROFILE_ZONE("phys.step");
    const i64 t0 = now_ns();
    fractures_this_frame_ = 0;

    broadphase();
    narrowphase();

    const u32 sub = maxu(1, cfg_.substeps);
    const float dts = dt / (float)sub;
    for (u32 s = 0; s < sub; ++s) {
        integrate(dts);
        solve(dts);
        integrate_positions(dts);
    }

    islands_and_sleep(dt);
    detect_fractures();
    process_fractures();

    stats_.bodies = (u32)body_count();
    stats_.step_ms = (float)(now_ns() - t0) / 1e6f;
}

} // namespace mtt::physics
