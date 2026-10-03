// Copyright 2025-2026 Yi-Ping Pan (Cloudlet)
//
// Property-based check: the real engine against the independent reference in
// reference_scheduler.hpp, over many random programs. This checks the
// scheduling rule itself — resources, dependencies, priorities, capacity,
// ties — the layer where the O(Q^2) requeue bug lived.
//
// Three things this deliberately does not attempt, and why:
//
// - A node's own dynamic cost (cache hit or miss). Reference-modelling a
//   stateful cache would mean re-deriving its hit/miss decisions independently
//   too, which is a different, much larger undertaking than re-deriving the
//   scheduling rule — and it is already covered by the hand-computed cache
//   tests and by comparing the full record set against a prior commit.
// - Submission in windows. reference_scheduler.hpp answers for the whole
//   program at once; a windowed run's equivalence to that is already its own
//   property, checked directly in
//   EventEngine.RunningInWindowsMatchesRunningAllAtOnce with real multi-agent
//   contention, rather than duplicated here.
// - Shrinking a failing program to a minimal one. Every program here is fully
//   determined by its seed, so a failure is reproducible — re-run with that
//   seed — even though it is not automatically minimised.
//
// initiator is attributed per op for realism (Record::initiator), though the
// engine never reads it when scheduling — see EventEngine::Start/Finish — so
// it cannot itself be a source of disagreement between the two.

#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "gtest/gtest.h"
#include "reference_scheduler.hpp"
#include "timeball/event_engine.hpp"

using namespace timeball;

namespace {

struct RandomProgram {
  std::vector<reference::SpecOp> spec;
  std::vector<std::uint32_t> capacities;
};

// Every program is fully determined by its seed, so a failure is reproducible
// by re-running with the same seed rather than needing to be captured.
RandomProgram GenerateProgram(std::uint32_t seed, int op_count,
                              int resource_count, bool multi_resource = false) {
  std::mt19937 rng(seed);
  std::uniform_int_distribution<int> resource_dist(0, resource_count - 1);
  std::uniform_int_distribution<reference::Cycle> ready_dist(0, 20);
  std::uniform_int_distribution<reference::Cycle> duration_dist(1, 15);
  std::uniform_int_distribution<int> priority_dist(0, 2);
  std::uniform_int_distribution<std::uint32_t> capacity_dist(1, 3);
  std::uniform_int_distribution<std::uint32_t> initiator_dist(0, 2);
  std::uniform_real_distribution<double> dep_chance(0.0, 1.0);

  RandomProgram program;
  program.capacities.resize(static_cast<std::size_t>(resource_count));
  for (auto& cap : program.capacities) {
    cap = capacity_dist(rng);
  }

  for (int i = 0; i < op_count; ++i) {
    reference::SpecOp op;
    op.resource = resource_dist(rng);
    op.ready_cycle = ready_dist(rng);
    op.duration = duration_dist(rng);
    op.priority = static_cast<std::uint32_t>(priority_dist(rng));
    op.initiator = initiator_dist(rng);
    if (multi_resource) {
      for (int r = 0; r < resource_count; ++r) {
        if (r != op.resource && dep_chance(rng) < 0.3) {
          op.additional_resources.push_back(r);
        }
      }
    }
    // Each earlier op is an independent chance to become a dependency, so
    // fan-in and chain depth both vary across programs.
    for (int d = 0; d < i; ++d) {
      if (dep_chance(rng) < 0.15) {
        op.deps.push_back(d);
      }
    }
    program.spec.push_back(std::move(op));
  }
  return program;
}

// Submits every op in index order, so an op's index equals its submission
// order — the same tie-break key the engine and the reference both use.
std::vector<reference::SpecResult> RunOnRealEngine(
    const RandomProgram& program) {
  EventEngine engine;
  std::vector<ResourceId> resources;
  for (std::size_t r = 0; r < program.capacities.size(); ++r) {
    resources.push_back(
        engine.AddResource({"r" + std::to_string(r), program.capacities[r]}));
  }

  std::vector<EventId> ids;
  ids.reserve(program.spec.size());
  for (const auto& op : program.spec) {
    std::vector<EventId> deps;
    deps.reserve(op.deps.size());
    for (int d : op.deps) {
      deps.push_back(ids[static_cast<std::size_t>(d)]);
    }
    std::vector<ResourceId> additional;
    for (int r : op.additional_resources) {
      additional.push_back(resources[static_cast<std::size_t>(r)]);
    }
    ids.push_back(
        engine.Submit({"op",
                       resources[static_cast<std::size_t>(op.resource)],
                       op.initiator,
                       op.duration,
                       {op.ready_cycle, std::move(deps), op.priority},
                       std::move(additional)}));
  }

  RecordingSink trace;
  engine.RunUntilIdle(&trace);
  std::unordered_map<EventId, reference::SpecResult> by_id;
  for (const auto& r : trace.records) {
    by_id[r.op] = {r.start.value(), r.finish.value()};  // one record per op
  }

  std::vector<reference::SpecResult> results;
  results.reserve(ids.size());
  for (EventId id : ids) {
    results.push_back(by_id.at(id));
  }
  return results;
}

}  // namespace

TEST(ReferenceScheduler, MatchesTheRealEngineOnManyRandomPrograms) {
  constexpr std::uint32_t kPrograms = 300;
  constexpr int kOpsPerProgram = 20;
  constexpr int kResources = 4;

  for (std::uint32_t seed = 0; seed < kPrograms; ++seed) {
    const RandomProgram program =
        GenerateProgram(seed, kOpsPerProgram, kResources);
    const auto expected = reference::Schedule(program.spec, program.capacities);
    ASSERT_TRUE(expected.has_value())
        << "reference scheduler did not converge for seed " << seed;
    const auto actual = RunOnRealEngine(program);
    ASSERT_EQ(actual.size(), expected->size());
    for (std::size_t i = 0; i < actual.size(); ++i) {
      EXPECT_EQ(actual[i].start, (*expected)[i].start)
          << "seed " << seed << " op " << i << " start";
      EXPECT_EQ(actual[i].finish, (*expected)[i].finish)
          << "seed " << seed << " op " << i << " finish";
    }
  }
}

TEST(ReferenceScheduler, MatchesAtomicResourceSetsOnManyRandomPrograms) {
  for (std::uint32_t seed = 0; seed < 300; ++seed) {
    const RandomProgram program = GenerateProgram(seed, 20, 4, true);
    const auto expected = reference::Schedule(program.spec, program.capacities);
    ASSERT_TRUE(expected.has_value()) << "seed " << seed;
    const auto actual = RunOnRealEngine(program);
    ASSERT_EQ(actual.size(), expected->size());
    for (std::size_t i = 0; i < actual.size(); ++i) {
      EXPECT_EQ(actual[i].start, (*expected)[i].start)
          << "seed " << seed << " op " << i;
      EXPECT_EQ(actual[i].finish, (*expected)[i].finish)
          << "seed " << seed << " op " << i;
    }
  }
}
