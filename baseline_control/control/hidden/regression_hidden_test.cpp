#include "rs/scheduler/dispatcher.h"
#include "rs/scheduler/queue.h"
#include "rs/scheduler/request_scheduler.h"
#include "rs/util/validation.h"
#include "test_support.h"

int main() {
    rs::scheduler::Queue queue;
    queue.push({"valid-1", 2, 0, true});
    queue.push({"valid_2", 1, 1, false});
    require(queue.size() == 2, "queue retains inserted jobs");
    require(rs::util::valid_jobs(queue.snapshot()), "fixture ids remain valid");

    rs::scheduler::RequestScheduler scheduler(rs::config::default_config());
    const auto ids = rs::scheduler::dispatch_ids(scheduler.schedule(queue.snapshot()));
    require(ids.size() == 1 && ids[0] == "valid-1",
            "dispatch emits only scheduled jobs");
    return 0;
}
