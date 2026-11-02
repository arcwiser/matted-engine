// Matted Engine - tests entry point
#include "test_framework.h"

#include "engine/core/jobs/job_system.h"

int main() {
    // The job pool backs RenderQueue::build and several tests; init once up
    // front (init is idempotent) and keep it alive for the whole run.
    mtt::jobs::init();
    const int rc = mtttest::run_all();
    mtt::jobs::shutdown();
    return rc;
}
