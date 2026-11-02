// Matted Engine - entity registry (component storage)
//
// Data-oriented ECS: components live in dense arrays (cache-friendly iteration);
// entity ids are generational (20-bit index + 12-bit version) so stale handles
// are detected instead of silently aliasing recycled entities.
#pragma once
#include <cassert>
#include <typeindex>
#include <unordered_map>
#include <vector>
#include "engine/core/mtt_types.h"

namespace mtt::ecs {

class Registry {
public:
    // ------------------------------------------------------------- entities
    Entity create() {
        u32 idx;
        if (!free_list_.empty()) {
            idx = free_list_.back();
            free_list_.pop_back();
        } else {
            idx = (u32)versions_.size();
            versions_.push_back(1);
        }
        Entity e;
        e.id = idx | (versions_[idx] << kEntityIndexBits);
        return e;
    }
    void destroy(Entity e) {
        if (!alive(e)) return;
        for (auto& [type, pool] : pools_) pool->remove_if_has(e);
        versions_[e.index()]++; // invalidates all outstanding handles
        free_list_.push_back(e.index());
    }
    bool alive(Entity e) const {
        if (!e.valid() || e.index() >= versions_.size()) return false;
        return versions_[e.index()] == e.version();
    }
    size_t size() const { return versions_.size() - free_list_.size(); }

    // ------------------------------------------------------------- components
    template <typename T>
    T& emplace(Entity e, T value = T{}) {
        assert(alive(e));
        Pool<T>& p = pool<T>();
        if (T* existing = p.try_get(e)) { *existing = std::move(value); return *existing; }
        return p.add(e, std::move(value));
    }
    template <typename T>
    T* try_get(Entity e) {
        if (!alive(e)) return nullptr;
        return pool<T>().try_get(e);
    }
    template <typename T>
    const T* try_get(Entity e) const {
        if (!alive(e)) return nullptr;
        return pool<T>().try_get(e);
    }
    template <typename T>
    T& get(Entity e) {
        T* p = try_get<T>(e);
        assert(p && "component missing");
        return *p;
    }
    template <typename T>
    bool has(Entity e) const { return try_get<T>(e) != nullptr; }
    template <typename T>
    void remove(Entity e) { pool<T>().remove_if_has(e); }

    // Dense iteration over one component type (fastest path).
    template <typename T, typename Fn>
    void each(Fn&& fn) {
        Pool<T>& p = pool<T>();
        for (size_t i = 0; i < p.dense_entities.size(); ++i)
            fn(p.dense_entities[i], p.dense[i]);
    }

    // Two-component view: iterates pool A, requires B. Yields (entity, A&, B&).
    template <typename A, typename B, typename Fn>
    void view(Fn&& fn) { pair_view<A, B>(fn); }

    size_t count_of(std::type_index t) const {
        auto it = pools_.find(t);
        return it == pools_.end() ? 0 : it->second->size();
    }

private:
    struct IPool {
        virtual ~IPool() = default;
        virtual void remove_if_has(Entity e) = 0;
        virtual size_t size() const = 0;
    };

    template <typename T>
    struct Pool final : IPool {
        std::vector<T> dense;                    // component data, packed
        std::vector<Entity> dense_entities;      // parallel owner ids
        std::unordered_map<u32, size_t> sparse;  // entity index -> dense slot

        T* try_get(Entity e) {
            auto it = sparse.find(e.index());
            if (it == sparse.end()) return nullptr;
            if (dense_entities[it->second] != e) return nullptr; // stale
            return &dense[it->second];
        }
        const T* try_get(Entity e) const {
            auto it = sparse.find(e.index());
            if (it == sparse.end()) return nullptr;
            if (dense_entities[it->second] != e) return nullptr;
            return &dense[it->second];
        }
        T& add(Entity e, T&& value) {
            sparse[e.index()] = dense.size();
            dense.push_back(std::move(value));
            dense_entities.push_back(e);
            return dense.back();
        }
        void remove_if_has(Entity e) override {
            auto it = sparse.find(e.index());
            if (it == sparse.end()) return;
            if (dense_entities[it->second] != e) return;
            const size_t slot = it->second;
            const size_t last = dense.size() - 1;
            if (slot != last) { // swap-remove
                dense[slot] = std::move(dense[last]);
                dense_entities[slot] = dense_entities[last];
                sparse[dense_entities[slot].index()] = slot;
            }
            dense.pop_back();
            dense_entities.pop_back();
            sparse.erase(it);
        }
        size_t size() const override { return dense.size(); }
    };

    template <typename T>
    Pool<T>& pool() {
        auto it = pools_.find(std::type_index(typeid(T)));
        if (it == pools_.end()) {
            auto up = std::make_unique<Pool<T>>();
            Pool<T>* raw = up.get();
            pools_.emplace(std::type_index(typeid(T)), std::move(up));
            return *raw;
        }
        return *static_cast<Pool<T>*>(it->second.get());
    }
    template <typename T>
    const Pool<T>& pool() const {
        return *static_cast<const Pool<T>*>(pools_.at(std::type_index(typeid(T))).get());
    }

    template <typename... Ts>
    bool has_all(Entity e) const { return (has<Ts>(e) && ...); }

    template <typename A, typename B, typename Fn>
    void pair_view(Fn&& fn) {
        Pool<A>& pa = pool<A>();
        for (size_t i = pa.dense_entities.size(); i-- > 0;) {
            Entity e = pa.dense_entities[i];
            if (!alive(e) || !has<B>(e)) continue;
            fn(e, *pa.try_get(e), *try_get<B>(e));
        }
    }

private:
    std::unordered_map<std::type_index, std::unique_ptr<IPool>> pools_;
    std::vector<u32> versions_;
    std::vector<u32> free_list_;
};

} // namespace mtt::ecs
