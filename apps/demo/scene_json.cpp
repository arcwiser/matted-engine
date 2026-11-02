// Matted Engine - JSON scene save/load implementation (see scene_json.h).
#include "scene_json.h"

#include <cstdio>
#include <cstring>

namespace mtt::scene {
namespace {

// ------------------------------------------------------------------ helpers
json::Value v3(vec3 v) {
    json::Value a = json::Value::array();
    a.push(json::Value::number((double)v.x));
    a.push(json::Value::number((double)v.y));
    a.push(json::Value::number((double)v.z));
    return a;
}
json::Value v4(float x, float y, float z, float w) {
    json::Value a = json::Value::array();
    a.push(json::Value::number((double)x));
    a.push(json::Value::number((double)y));
    a.push(json::Value::number((double)z));
    a.push(json::Value::number((double)w));
    return a;
}
vec3 read_v3(const json::Value& v, vec3 def = {}) {
    if (!v.is_array() || v.items().size() < 3) return def;
    return {v.items()[0].as_float(def.x), v.items()[1].as_float(def.y),
            v.items()[2].as_float(def.z)};
}
quat read_quat(const json::Value& v, quat def) {
    if (!v.is_array() || v.items().size() < 4) return def;
    return {v.items()[0].as_float(), v.items()[1].as_float(),
            v.items()[2].as_float(), v.items()[3].as_float()};
}

// ------------------------------------------------------------------ camera/world
json::Value save_camera(const Camera& c) {
    json::Value o = json::Value::object();
    o.set("pos", v3(c.pos));
    o.set("yaw", json::Value::number((double)c.yaw));
    o.set("pitch", json::Value::number((double)c.pitch));
    o.set("fov_y_deg", json::Value::number((double)(c.fov_y * kRadToDeg)));
    o.set("near_z", json::Value::number((double)c.near_z));
    o.set("far_z", json::Value::number((double)c.far_z));
    return o;
}
void load_camera(const json::Value& o, Camera& c) {
    c.pos    = read_v3(o.get("pos"), c.pos);
    c.yaw    = o.get("yaw").as_float(c.yaw);
    c.pitch  = o.get("pitch").as_float(c.pitch);
    c.fov_y  = o.get("fov_y_deg").as_float(c.fov_y * kRadToDeg) * kDegToRad;
    c.near_z = o.get("near_z").as_float(c.near_z);
    c.far_z  = o.get("far_z").as_float(c.far_z);
}

json::Value save_world(const physics::WorldConfig& w) {
    json::Value o = json::Value::object();
    o.set("gravity", v3(w.gravity));
    o.set("substeps", json::Value::number((double)w.substeps));
    o.set("velocity_iterations", json::Value::number((double)w.velocity_iterations));
    o.set("baumgarte", json::Value::number((double)w.baumgarte));
    o.set("penetration_slop", json::Value::number((double)w.penetration_slop));
    o.set("max_correction_vel", json::Value::number((double)w.max_correction_vel));
    o.set("rest_vel_threshold", json::Value::number((double)w.rest_vel_threshold));
    o.set("sleep_lin_tol", json::Value::number((double)w.sleep_lin_tol));
    o.set("sleep_ang_tol", json::Value::number((double)w.sleep_ang_tol));
    o.set("sleep_time", json::Value::number((double)w.sleep_time));
    json::Value fr = json::Value::object();
    fr.set("enabled", json::Value::boolean(w.fracture_enabled));
    fr.set("impulse_base", json::Value::number((double)w.fracture_impulse_base));
    fr.set("impulse_growth", json::Value::number((double)w.fracture_impulse_growth));
    fr.set("max_depth", json::Value::number((double)w.fracture_max_depth));
    fr.set("min_size", json::Value::number((double)w.fracture_min_size));
    fr.set("splits", json::Value::number((double)w.fracture_splits));
    o.set("fracture", fr);
    return o;
}
void load_world(const json::Value& o, physics::WorldConfig& w) {
    w.gravity             = read_v3(o.get("gravity"), w.gravity);
    w.substeps            = o.get("substeps").as_u32(w.substeps);
    w.velocity_iterations = o.get("velocity_iterations").as_u32(w.velocity_iterations);
    w.baumgarte           = o.get("baumgarte").as_float(w.baumgarte);
    w.penetration_slop    = o.get("penetration_slop").as_float(w.penetration_slop);
    w.max_correction_vel  = o.get("max_correction_vel").as_float(w.max_correction_vel);
    w.rest_vel_threshold  = o.get("rest_vel_threshold").as_float(w.rest_vel_threshold);
    w.sleep_lin_tol       = o.get("sleep_lin_tol").as_float(w.sleep_lin_tol);
    w.sleep_ang_tol       = o.get("sleep_ang_tol").as_float(w.sleep_ang_tol);
    w.sleep_time          = o.get("sleep_time").as_float(w.sleep_time);
    const json::Value& fr = o.get("fracture");
    if (fr.is_object()) {
        w.fracture_enabled        = fr.get("enabled").as_bool(w.fracture_enabled);
        w.fracture_impulse_base   = fr.get("impulse_base").as_float(w.fracture_impulse_base);
        w.fracture_impulse_growth = fr.get("impulse_growth").as_float(w.fracture_impulse_growth);
        w.fracture_max_depth      = fr.get("max_depth").as_u32(w.fracture_max_depth);
        w.fracture_min_size       = fr.get("min_size").as_float(w.fracture_min_size);
        w.fracture_splits         = fr.get("splits").as_u32(w.fracture_splits);
    }
}

// ------------------------------------------------------------------ mesh gens
std::unordered_map<std::string, MeshGenFn>& gens() {
    static std::unordered_map<std::string, MeshGenFn> g;
    static bool init = false;
    if (!init) {
        init = true;
        g.emplace("cube",      [](const json::Value&) { return meshgen::cube(); });
        g.emplace("cube_lod1", [](const json::Value&) { return meshgen::cube_lod1(); });
        g.emplace("uv_sphere", [](const json::Value& p) {
            return meshgen::uv_sphere(p.get("segs").as_int(12), p.get("rings").as_int(8));
        });
        g.emplace("plane_grid", [](const json::Value& p) {
            return meshgen::plane_grid(p.get("n").as_int(1), p.get("size").as_float(2.0f),
                                       p.get("repeat").as_u32(0));
        });
        g.emplace("quad_xy",   [](const json::Value& p) { return meshgen::quad_xy(p.get("size").as_float(2.0f)); });
    }
    return g;
}

std::unordered_map<std::string, size_t>& source_index() {
    static std::unordered_map<std::string, size_t> idx;
    return idx;
}
std::vector<MeshSource>& sources() {
    static std::vector<MeshSource> s;
    return s;
}

json::Value save_decl(const MeshDecl& d) {
    json::Value o = json::Value::object();
    o.set("gen", json::Value::string(d.gen));
    o.set("params", d.params);
    return o;
}
MeshDecl load_decl(const json::Value& o) {
    MeshDecl d;
    d.gen    = o.get("gen").as_string();
    d.params = o.get("params");
    return d;
}

} // namespace

// ------------------------------------------------------------------ registry
void register_mesh_gen(const char* gen_name, MeshGenFn fn) {
    gens()[gen_name] = std::move(fn);
}
const MeshGenFn* find_mesh_gen(const char* gen_name) {
    auto& g = gens();
    auto it = g.find(gen_name);
    return it == g.end() ? nullptr : &it->second;
}

void declare_mesh_source(const MeshSource& src) {
    auto& idx = source_index();
    auto it = idx.find(src.lib_name);
    if (it != idx.end()) {
        sources()[it->second] = src;
        return;
    }
    idx[src.lib_name] = sources().size();
    sources().push_back(src);
}
const MeshSource* find_mesh_source(const char* lib_name) {
    auto& idx = source_index();
    auto it = idx.find(lib_name);
    return it == idx.end() ? nullptr : &sources()[it->second];
}
void clear_mesh_sources() {
    sources().clear();
    source_index().clear();
}

void declare_builtin_mesh(const char* lib_name, const char* gen, json::Value params) {
    MeshSource src;
    src.lib_name = lib_name;
    src.base.gen = gen;
    src.base.params = std::move(params);
    if (std::strcmp(gen, "cube") == 0) {   // the engine's one built-in LOD chain
        src.has_lod1 = true;
        src.lod1.gen = "cube_lod1";
    }
    declare_mesh_source(src);
}

// ------------------------------------------------------------------ save
bool save_scene(const SceneSave& scene, const char* path, std::string* err) {
    json::Value root = json::Value::object();
    root.set("format", json::Value::string("matted-scene"));
    root.set("version", json::Value::number(1));
    root.set("camera", save_camera(scene.camera));
    root.set("world", save_world(scene.world));

    json::Value mesh_list = json::Value::array();
    for (const MeshSource& src : scene.meshes) {
        json::Value o = json::Value::object();
        o.set("name", json::Value::string(src.lib_name));
        o.set("base", save_decl(src.base));
        o.set("lod1", src.has_lod1 ? save_decl(src.lod1) : json::Value::null());
        o.set("lod2", src.has_lod2 ? save_decl(src.lod2) : json::Value::null());
        mesh_list.push(std::move(o));
    }
    root.set("meshes", std::move(mesh_list));

    json::Value mats = json::Value::array();
    for (const auto& [name, m] : scene.materials) {
        json::Value o = json::Value::object();
        o.set("name", json::Value::string(name));
        o.set("albedo", v3(m.albedo));
        o.set("checker", json::Value::number((double)m.checker));
        o.set("checker_dim", json::Value::number((double)m.checker_dim));
        mats.push(std::move(o));
    }
    root.set("materials", std::move(mats));

    json::Value insts = json::Value::array();
    for (const InstanceSave& is : scene.instances) {
        json::Value o = json::Value::object();
        o.set("mesh", json::Value::string(is.mesh_name));
        o.set("lod1_dist", json::Value::number((double)is.lod.lod1_dist));
        o.set("lod2_dist", json::Value::number((double)is.lod.lod2_dist));
        o.set("material", json::Value::string(is.material));
        o.set("pos", v3(is.pos));
        o.set("rot", v4(is.rot.x, is.rot.y, is.rot.z, is.rot.w));
        o.set("scale", v3(is.scale));
        if (is.has_body) {
            const physics::Body& b = is.body;
            json::Value bo = json::Value::object();
            switch (b.shape.type) {
                case physics::ShapeType::Sphere: bo.set("shape", json::Value::string("sphere")); break;
                case physics::ShapeType::Box:    bo.set("shape", json::Value::string("box")); break;
                case physics::ShapeType::Plane:  bo.set("shape", json::Value::string("plane")); break;
            }
            bo.set("radius", json::Value::number((double)b.shape.radius));
            bo.set("half", v3(b.shape.half));
            bo.set("plane_n", v3(b.shape.plane_n));
            bo.set("plane_d", json::Value::number((double)b.shape.plane_d));
            bo.set("pos", v3(b.pos));
            bo.set("rot", v4(b.rot.x, b.rot.y, b.rot.z, b.rot.w));
            bo.set("mass", json::Value::number(b.inv_mass > 0.0f ? (double)(1.0f / b.inv_mass) : 0.0));
            bo.set("lin_vel", v3(b.lin_vel));
            bo.set("ang_vel", v3(b.ang_vel));
            bo.set("friction", json::Value::number((double)b.mat.friction));
            bo.set("restitution", json::Value::number((double)b.mat.restitution));
            bo.set("static", json::Value::boolean(b.is_static()));
            bo.set("can_sleep", json::Value::boolean((b.flags & physics::kBodyCanSleep) != 0));
            bo.set("fracturable", json::Value::boolean((b.flags & physics::kBodyFracturable) != 0));
            bo.set("depth", json::Value::number((double)b.depth));
            o.set("body", std::move(bo));
        }
        insts.push(std::move(o));
    }
    root.set("instances", std::move(insts));

    std::string out;
    json::write(root, out);

    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") != 0 || !f) {
        if (err) *err = std::string("cannot open for write: ") + path;
        return false;
    }
    const size_t written = fwrite(out.data(), 1, out.size(), f);
    const bool ok = (written == out.size());
    if (!ok && err) *err = "short write";
    fclose(f);
    return ok;
}

// ------------------------------------------------------------------ load
bool load_scene_from_memory(const char* text, size_t len, SceneSave& scene,
                            std::string* err, std::vector<std::string>* warnings) {
    json::Value root;
    if (!json::parse(text, len, root, err)) return false;
    if (!root.is_object() || root.get("format").as_string() != "matted-scene") {
        if (err) *err = "not a matted-scene document";
        return false;
    }
    const int version = root.get("version").as_int(0);
    if (version != 1) {
        if (err) *err = "unsupported scene version " + std::to_string(version);
        return false;
    }

    scene = SceneSave{};
    load_camera(root.get("camera"), scene.camera);
    load_world(root.get("world"), scene.world);

    for (const json::Value& m : root.get("meshes").items()) {
        MeshSource src;
        src.lib_name = m.get("name").as_string();
        src.base     = load_decl(m.get("base"));
        const json::Value& l1 = m.get("lod1");
        const json::Value& l2 = m.get("lod2");
        if (l1.is_object()) { src.lod1 = load_decl(l1); src.has_lod1 = true; }
        if (l2.is_object()) { src.lod2 = load_decl(l2); src.has_lod2 = true; }
        scene.meshes.push_back(std::move(src));
    }

    for (const json::Value& m : root.get("materials").items()) {
        Material mat;
        mat.albedo      = read_v3(m.get("albedo"), mat.albedo);
        mat.checker     = m.get("checker").as_float(mat.checker);
        mat.checker_dim = m.get("checker_dim").as_float(mat.checker_dim);
        scene.materials.emplace_back(m.get("name").as_string(), mat);
    }

    for (const json::Value& o : root.get("instances").items()) {
        InstanceSave is;
        is.mesh_name = o.get("mesh").as_string();
        is.lod.lod1_dist = o.get("lod1_dist").as_float(is.lod.lod1_dist);
        is.lod.lod2_dist = o.get("lod2_dist").as_float(is.lod.lod2_dist);
        is.material  = o.get("material").as_string();
        is.pos       = read_v3(o.get("pos"), is.pos);
        is.rot       = read_quat(o.get("rot"), is.rot);
        is.scale     = read_v3(o.get("scale"), is.scale);

        const json::Value& bo = o.get("body");
        if (bo.is_object()) {
            physics::Body b;
            std::string shape = bo.get("shape").as_string();
            if (shape.empty()) shape = "sphere";
            if      (shape == "sphere") b.shape = physics::Shape::sphere(bo.get("radius").as_float(0.5f));
            else if (shape == "box")    b.shape = physics::Shape::box(read_v3(bo.get("half"), vec3(0.5f, 0.5f, 0.5f)));
            else if (shape == "plane")  b.shape = physics::Shape::plane(read_v3(bo.get("plane_n"), vec3(0, 1, 0)),
                                                                        bo.get("plane_d").as_float());
            else if (warnings) warnings->push_back("instance '" + is.mesh_name + "': unknown shape '" + shape + "' -> sphere");

            const float mass = bo.get("mass").as_float(1.0f);
            b.inv_mass = mass > 0.0f ? 1.0f / mass : 0.0f;
            // body transform defaults to the instance transform when absent
            // (hand-edited files); body pos wins at restore time either way
            b.pos      = read_v3(bo.get("pos"), is.pos);
            b.rot      = read_quat(bo.get("rot"), is.rot);
            b.lin_vel  = read_v3(bo.get("lin_vel"), b.lin_vel);
            b.ang_vel  = read_v3(bo.get("ang_vel"), b.ang_vel);
            b.mat.friction    = bo.get("friction").as_float(b.mat.friction);
            b.mat.restitution = bo.get("restitution").as_float(b.mat.restitution);
            b.depth = bo.get("depth").as_u32(b.depth);

            u32 flags = 0;
            const bool static_body = mass <= 0.0f || bo.get("static").as_bool(false);
            if (static_body)      flags |= physics::kBodyStatic;
            if (bo.get("can_sleep").as_bool(true))    flags |= physics::kBodyCanSleep;
            if (bo.get("fracturable").as_bool(false)) flags |= physics::kBodyFracturable;
            b.flags = flags;

            is.has_body = true;
            is.body = b;
        }
        scene.instances.push_back(std::move(is));
    }

    if (err) err->clear();
    return true;
}

bool load_scene(const char* path, SceneSave& scene, std::string* err,
                std::vector<std::string>* warnings) {
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f) {
        if (err) *err = std::string("cannot open for read: ") + path;
        return false;
    }
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    const bool io_error = ferror(f) != 0;
    fclose(f);
    if (io_error) {
        if (err) *err = std::string("read error: ") + path;
        return false;
    }
    return load_scene_from_memory(text.data(), text.size(), scene, err, warnings);
}

// ------------------------------------------------------------------ restore
void rebuild_meshes(const std::vector<MeshSource>& srcs, MeshLibrary& lib,
                    std::vector<std::string>* warnings) {
    for (const MeshSource& src : srcs) {
        MeshData base;
        const MeshGenFn* fn = find_mesh_gen(src.base.gen.c_str());
        if (fn) base = (*fn)(src.base.params);
        else if (warnings) warnings->push_back("mesh '" + src.lib_name + "': unknown generator '" + src.base.gen + "'");

        MeshId id = MeshId(-1);
        for (size_t i = 0; i < lib.names_.size(); ++i)
            if (lib.names_[i] == src.lib_name) { id = (MeshId)i; break; }
        if (id == MeshId(-1)) {
            id = lib.add(std::move(base), src.lib_name.c_str());
        } else {
            lib.meshes_[id] = std::move(base);   // replace in place
        }

        const MeshDecl* lods[2] = {&src.lod1, &src.lod2};
        const bool has[2] = {src.has_lod1, src.has_lod2};
        for (u32 i = 0; i < 2; ++i) {
            if (!has[i]) continue;
            const u32 slot = i + 1; // 1 or 2
            const MeshGenFn* lfn = find_mesh_gen(lods[i]->gen.c_str());
            if (!lfn) {
                if (warnings) warnings->push_back("mesh '" + src.lib_name + "' lod" +
                                                  std::to_string(slot) + ": unknown generator '" + lods[i]->gen + "'");
                continue;
            }
            // add_lod pushes a NEW entry each call; if a lod entry already
            // exists for this slot, replace it in place to keep the library
            // size stable across repeated restores.
            const MeshId existing = lib.lods_[id][slot];
            if (existing != kInvalid32 && existing < lib.meshes_.size()) {
                lib.meshes_[existing] = (*lfn)(lods[i]->params);
            } else {
                lib.add_lod(id, slot, (*lfn)(lods[i]->params));
            }
        }
    }
}

bool restore_objects(const SceneSave& in, MeshLibrary& lib, MaterialLibrary& mats,
                     RestoredScene& out, std::vector<std::string>* warnings) {
    out = RestoredScene{};

    rebuild_meshes(in.meshes, lib, warnings);

    // material ids by name (reuse existing id when the library already has it,
    // so restoring into the demo's populated library does not duplicate)
    std::unordered_map<std::string, MaterialId> mat_ids;
    for (const auto& [name, m] : in.materials) {
        MaterialId id = MaterialId(-1);
        for (size_t i = 0; i < mats.names_.size(); ++i)
            if (mats.names_[i] == name) { id = (MaterialId)i; break; }
        if (id == MaterialId(-1)) id = mats.add(Material(m), name.c_str());
        mat_ids[name] = id;
    }

    // pass 1: resolve all refs first so the vector stops growing before any
    // RenderInstance takes a MeshRef* into it (pointer stability)
    std::unordered_map<std::string, u32> ref_ids;
    auto ref_for = [&](const std::string& mesh_name, const LodSettings& lod) -> u32 {
        const std::string key = mesh_name + "|" + std::to_string(lod.lod1_dist) + "|" +
                                std::to_string(lod.lod2_dist);
        auto it = ref_ids.find(key);
        if (it != ref_ids.end()) return it->second;

        MeshId mesh = MeshId(-1);
        for (size_t i = 0; i < lib.names_.size(); ++i)
            if (lib.names_[i] == mesh_name) { mesh = (MeshId)i; break; }
        if (mesh == MeshId(-1)) {
            if (warnings) warnings->push_back("instance: unknown mesh '" + mesh_name + "' -> empty mesh");
            mesh = lib.add(MeshData{}, mesh_name.c_str());
        }
        const u32 idx = (u32)out.refs.size();
        out.refs.push_back(MeshRef{mesh, lod});
        ref_ids[key] = idx;
        return idx;
    };
    for (const InstanceSave& is : in.instances) (void)ref_for(is.mesh_name, is.lod);

    // pass 2: instances (refs vector is now stable)
    out.instances.reserve(in.instances.size());
    out.bodies.reserve(in.instances.size());
    out.has_body.reserve(in.instances.size());
    for (const InstanceSave& is : in.instances) {
        RenderInstance ri;
        ri.pos = is.pos;
        ri.rot = is.rot;
        ri.scale = is.scale;
        ri.mesh_ref = &out.refs[ref_for(is.mesh_name, is.lod)];
        ri.material = mat_ids.count(is.material) ? mat_ids[is.material] : MaterialId(0);
        out.instances.push_back(ri);
        out.bodies.push_back(is.body);
        out.has_body.push_back(is.has_body);
    }
    return true;
}

InstanceSave make_instance_save(const RenderInstance& ri, const physics::Body* body,
                                const MeshLibrary& meshes) {
    InstanceSave is;
    is.pos = ri.pos;
    is.rot = ri.rot;
    is.scale = ri.scale;
    is.lod = ri.mesh_ref ? ri.mesh_ref->lod : LodSettings{};
    is.mesh_name = (ri.mesh_ref && ri.mesh_ref->mesh < meshes.names_.size())
                       ? meshes.names_[ri.mesh_ref->mesh]
                       : "";
    if (body) {
        is.has_body = true;
        is.body = *body;
        is.body.sleep_timer = 0.0f;   // runtime state, never persisted
        is.pos = body->pos;           // body transform wins (demo syncs from physics)
        is.rot = body->rot;
    }
    return is;
}

} // namespace mtt::scene
