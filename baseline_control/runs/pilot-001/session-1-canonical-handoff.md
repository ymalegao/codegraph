# Session handoff

## Objective and success condition

Diagnose the scheduling failure, restore the invariant that an eligible
higher-priority job is never flushed after an eligible lower-priority job, and
leave later scheduler behavior changes for a subsequent session.

## Completed work

- Diagnosed the visible failure in `priority_order_test`.
- Confirmed `src/scheduler/flush_planner.cpp::build_flush_plan` filters eligible
  jobs, sorts them through `util::stable_priority_sort`, then emits them in that
  order.
- Found the invariant violation in
  `src/policy/priority_policy.cpp::higher_priority_first`: it used `<`, sorting
  lower numeric priorities before higher priorities.
- Changed the comparator to `lhs.priority > rhs.priority`.
- Equal-priority ordering remains governed by
  `src/policy/tie_break_policy.cpp::earlier_enqueue_first`.

## Verification

- Before the change, the project built and 4/5 visible tests passed;
  `priority_order_test` failed at "highest-priority job must flush first".
- After the change, `cmake --build build` succeeded and
  `ctest --test-dir build --output-on-failure` passed all 5/5 visible tests.
- `git diff --check` passed.

## Next action

Implement and test the next explicitly staged behavior:

1. Update `src/policy/batch_policy.cpp::selectable_count` to enforce
   `SchedulerConfig::max_batch_size`, preserving `0` as unlimited (consistent
   with `batch_has_capacity`).
2. Add focused tests for finite and unlimited batch sizes.
3. After batch limiting is correct, evaluate
   `src/config/config_rules.cpp::background_is_allowed` and integrate
   `defer_background_when_interactive` into eligibility/planning with tests.

Do not combine those follow-ups with the priority fix without first confirming
their intended semantics from the current code and tests.
