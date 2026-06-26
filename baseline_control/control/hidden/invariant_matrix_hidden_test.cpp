#include "rs/scheduler/request_scheduler.h"
#include "test_support.h"

int main() {
    rs::scheduler::RequestScheduler scheduler(rs::config::default_config());
    const auto plan = scheduler.schedule({
        {"p2-a", 2, 4, true},
        {"p7", 7, 3, true},
        {"p2-b", 2, 1, true},
        {"p0", 0, 0, true},
        {"p4", 4, 2, true},
    });
    for (std::size_t i = 1; i < plan.size(); ++i) {
        require(plan.jobs()[i - 1].job.priority >= plan.jobs()[i].job.priority,
                "priority must be monotonically non-increasing");
    }
    require(plan.jobs()[2].job.id == "p2-b", "tie order uses enqueue sequence");
    return 0;
}
