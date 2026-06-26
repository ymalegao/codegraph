# Task-chain ground truth

This file is controller-only. Do not expose it to either agent condition.

## Named invariant

A higher-priority eligible job is never flushed after a lower-priority eligible
job. Equal priority is ordered by ascending `enqueue_sequence`.

## Session 1

- Initial visible failure: `priority_order_test`.
- Root cause: `rs::policy::higher_priority_first` compares priorities ascending.
- Safest first repair: change the comparison to descending.
- Expected remaining task: make `rs::policy::selectable_count` honor
  `SchedulerConfig::max_batch_size`, where zero means unlimited.
- Handoff anchors expected to matter:
  - `src/policy/priority_policy.cpp` — `rs::policy::higher_priority_first`
  - `src/scheduler/flush_planner.cpp` — `rs::scheduler::build_flush_plan`
  - `src/policy/batch_policy.cpp` — `rs::policy::selectable_count`

## Session 2, after Drift 1

- Re-resolved primary target:
  `src/scheduler/plan_composer.cpp` —
  `rs::scheduler::PlanComposer::compose`.
- Expected repair: cap the already priority-sorted eligible sequence in
  `rs::policy::selectable_count`; add a visible batch-limit test.
- Expected remaining task: honor
  `SchedulerConfig::defer_background_when_interactive` while still allowing
  background-only queues to make progress.
- Handoff anchors expected to matter:
  - `src/scheduler/plan_composer.cpp` —
    `rs::scheduler::PlanComposer::compose`
  - `src/config/config_rules.cpp` —
    `rs::config::background_is_allowed`
  - `src/util/append_if_eligible.cpp` —
    `rs::util::append_if_eligible`

## Session 3, after Drift 2

- Both session-2 follow-up anchors are gone.
- Current replacement:
  `src/policy/eligibility_gate.cpp` —
  `rs::policy::EligibilityGate::allow`.
- Expected repair: reject enabled priority-zero jobs only when deferral is
  configured and at least one enabled positive-priority job exists.
- Final success: all visible tests and all five hidden tests pass.

The `gold/session-*.patch` files are one acceptable implementation, not a
required textual patch. Scoring is behavior-first.
