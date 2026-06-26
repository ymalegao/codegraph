#include "rs/scheduler/request_scheduler.h"
#include "test_support.h"

int main() {
    rs::config::SchedulerConfig config;
    config.max_batch_size = 2;
    rs::scheduler::RequestScheduler scheduler(config);
    const auto plan = scheduler.schedule({
        {"low", 1, 0, true},
        {"high", 8, 1, true},
        {"middle", 4, 2, true},
    });
    require(plan.size() == 2, "configured batch limit is enforced");
    require(plan.jobs()[0].job.id == "high", "limit keeps highest priority");
    require(plan.jobs()[1].job.id == "middle", "limit keeps next priority");
    return 0;
}
