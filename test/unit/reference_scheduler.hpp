// Copyright 2025-2026 Yi-Ping Pan (Cloudlet)
//
// A pure, deliberately independent re-derivation of the event engine's own rule
// — completion = max(dependencies complete, resource free) + cost, a resource
// served in arrival order — used only to check the real engine against it
// (test_reference_scheduler.cpp). It shares no code with EventEngine: no heap,
// no per-resource waiting queue, no windowing. It walks forward one cycle at a
// time and asks, literally, who is waiting and who is free, which is slow
// (O(cycles * ops)) and exactly the point: a bug shaped like the engine's own
// implementation cannot also be shaped like this one.
//
// Test-only. Not part of the library timeball ships.

#ifndef TIMEBALL_TEST_REFERENCE_SCHEDULER_HPP
#define TIMEBALL_TEST_REFERENCE_SCHEDULER_HPP

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

namespace timeball::reference {

using Cycle = std::uint64_t;

// One piece of work: a resource, a fixed cost, what it waits on, and where it
// falls in submission order — everything an Operation carries, spelled out so
// this file need not include event_engine.hpp.
struct SpecOp {
  int resource = 0;
  Cycle ready_cycle = 0;
  Cycle duration = 0;
  std::vector<int>
      deps;  // indices into the same vector, each less than this one's
  std::uint32_t priority = 0;
  // Reported on the record, never read by the scheduling rule — carried here
  // only so a random program can attribute work to more than one agent.
  std::uint32_t initiator = 0;
  std::vector<int> additional_resources;
};

struct SpecResult {
  Cycle start = 0;
  Cycle finish = 0;
};

// nullopt only if max_cycle was too small to let every op finish, which signals
// a bug in the caller's bound, not in the scheduler.
inline std::optional<std::vector<SpecResult>> Schedule(
    const std::vector<SpecOp>& ops,
    const std::vector<std::uint32_t>& capacities) {
  const std::size_t n = ops.size();
  std::vector<std::optional<Cycle>> arrival(n);
  std::vector<std::optional<Cycle>> started(n);
  std::vector<std::optional<Cycle>> finished(n);
  std::vector<std::vector<Cycle>> free_until(capacities.size());
  for (std::size_t r = 0; r < capacities.size(); ++r) {
    free_until[r].assign(capacities[r], Cycle{0});
  }

  std::size_t remaining = n;
  Cycle bound = 1;
  for (const auto& op : ops) {
    bound = std::max(bound, op.ready_cycle + op.duration);
  }
  // Generous: every op serialised on one resource, one after another.
  const Cycle max_cycle = bound * static_cast<Cycle>(n + 1) + 1;

  for (Cycle t = 0; remaining > 0 && t <= max_cycle; ++t) {
    // An op's arrival is knowable once every dependency is resolved — started
    // and finished are always assigned together below, in the same statement,
    // so checking one has a value is the same as checking the other, and this
    // is the true completion whether or not "now" has reached it yet.
    for (std::size_t i = 0; i < n; ++i) {
      if (arrival[i].has_value()) {
        continue;
      }
      const bool deps_resolved = std::ranges::all_of(ops[i].deps, [&](int d) {
        return finished[static_cast<std::size_t>(d)].has_value();
      });
      if (!deps_resolved) {
        continue;
      }
      Cycle a = ops[i].ready_cycle;
      for (int d : ops[i].deps) {
        a = std::max(a, *finished[static_cast<std::size_t>(d)]);
      }
      arrival[i] = a;
    }

    std::vector<std::size_t> waiting;
    for (std::size_t i = 0; i < n; ++i) {
      if (arrival[i].has_value() && *arrival[i] <= t &&
          !started[i].has_value()) {
        waiting.push_back(i);
      }
    }
    std::ranges::sort(waiting, [&](std::size_t x, std::size_t y) {
      if (*arrival[x] != *arrival[y]) {
        return *arrival[x] < *arrival[y];
      }
      if (ops[x].priority != ops[y].priority) {
        return ops[x].priority > ops[y].priority;
      }
      return x < y;
    });
    // An earlier waiter prevents overtaking on every resource it needs, but
    // reserves no capacity while another required resource is busy.
    std::vector<bool> blocked(capacities.size(), false);
    for (std::size_t i : waiting) {
      std::vector<int> required = ops[i].additional_resources;
      required.push_back(ops[i].resource);
      const bool free = std::ranges::all_of(required, [&](int r) {
        return !blocked[r] && *std::ranges::min_element(free_until[r]) <= t;
      });
      if (!free) {
        for (int r : required) {
          blocked[r] = true;
        }
        continue;
      }
      started[i] = t;
      finished[i] = t + ops[i].duration;
      for (int r : required) {
        *std::ranges::min_element(free_until[r]) = *finished[i];
      }
      --remaining;
    }
  }

  if (remaining > 0) {
    return std::nullopt;
  }
  std::vector<SpecResult> results(n);
  for (std::size_t i = 0; i < n; ++i) {
    results[i] = {*started[i], *finished[i]};
  }
  return results;
}

}  // namespace timeball::reference

#endif  // TIMEBALL_TEST_REFERENCE_SCHEDULER_HPP
