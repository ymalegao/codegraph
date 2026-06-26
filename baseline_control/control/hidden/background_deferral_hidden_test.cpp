#include "rs/scheduler/request_scheduler.h"
#include "test_support.h"

int main() {
    rs::config::SchedulerConfig config;
    config.defer_background_when_interactive = true;
    rs::scheduler::RequestScheduler scheduler(config);

    const auto mixed = scheduler.schedule({
        {"background", 0, 0, true},
        {"interactive", 5, 1, true},
    });
    require(mixed.size() == 1, "background work is deferred when interactive work exists");
    require(mixed.jobs()[0].job.id == "interactive", "interactive job remains");

    const auto only_background = scheduler.schedule({
        {"background-a", 0, 0, true},
        {"background-b", 0, 1, true},
    });
    require(only_background.size() == 2,
            "background work is allowed when no interactive work exists");
    return 0;
}
