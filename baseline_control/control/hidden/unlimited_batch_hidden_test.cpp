#include "rs/scheduler/request_scheduler.h"
#include "test_support.h"

int main() {
    rs::config::SchedulerConfig config;
    config.max_batch_size = 0;
    rs::scheduler::RequestScheduler scheduler(config);
    require(scheduler.schedule({
        {"a", 3, 0, true},
        {"b", 2, 1, true},
        {"c", 1, 2, true},
    }).size() == 3, "zero batch size means unlimited");
    return 0;
}
