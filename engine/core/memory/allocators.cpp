#include "engine/core/memory/allocators.h"

namespace mtt::mem {

Arena& scratch() {
    thread_local Arena g_scratch(1 << 20); // 1 MiB initial, grows to high-water
    return g_scratch;
}

} // namespace mtt::mem
