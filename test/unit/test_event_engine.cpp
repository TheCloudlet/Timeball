#include <algorithm>
#include <string>
#include <vector>

#include "checked_run.hpp"
#include "gtest/gtest.h"
#include "timeball/checking_sink.hpp"
#include "timeball/event_engine.hpp"
#include "timeball/record_query.hpp"
#include "timeball/timeball.hpp"

using namespace timeball;
using namespace timeball::test;

TEST(EventEngine, IndependentThenContendedThenDependentOperations) {
  EventEngine engine;
  const ResourceId core = engine.AddResource({"core", 1});
  const ResourceId dma = engine.AddResource({"dma", 1});

  engine.Submit({"load", dma, 0, 10, {0, {}}});
  engine.Submit({"compute", core, 1, 7, {0, {}}});

  RecordingSink trace;
  CheckingSink checker;  // kept across all three runs: "dependent" below
                         // depends on an op that completes in an earlier one
  const RunResult independent = RunChecked(engine, checker, &trace);
  EXPECT_EQ(independent.end_cycle, 10u);
  ASSERT_EQ(trace.records.size(), 2u);
  EXPECT_EQ(trace.records[0].start, 0u);
  EXPECT_EQ(trace.records[1].start, 0u);

  // Two operations on the same resource, submitted with the same ready
  // cycle: the second queues behind the first.
  engine.Submit({"dma_a", dma, 0, 4, {10, {}}});
  const EventId contended_b = engine.Submit({"dma_b", dma, 1, 3, {10, {}}});
  const RunResult contended = RunChecked(engine, checker, &trace);
  ASSERT_EQ(contended.completed.size(), 2u);
  EXPECT_EQ(contended.completed[0].service_cycle, 10u);
  EXPECT_EQ(contended.completed[1].service_cycle, 14u);

  // A dependent operation waits for its dependency's actual completion, not
  // its own ready cycle.
  engine.Submit({"dependent", core, 0, 2, {0, {contended_b}}});
  const RunResult dependency = RunChecked(engine, checker, &trace);
  ASSERT_EQ(dependency.completed.size(), 1u);
  EXPECT_EQ(dependency.completed[0].service_cycle, 17u);
  ReportViolations(checker);
}

TEST(EventEngine, SelectionOrdersByReadyCycleNotSubmissionOrder) {
  EventEngine engine;
  const ResourceId resource = engine.AddResource({"ready", 1});
  const EventId late = engine.Submit({"late", resource, 0, 1, {100, {}}});
  const EventId early = engine.Submit({"early", resource, 1, 1, {0, {}}});

  const RunResult run = RunChecked(engine);
  ASSERT_EQ(run.completed.size(), 2u);
  EXPECT_EQ(run.completed[0].event_id, early);
  EXPECT_EQ(run.completed[0].service_cycle, 0u);
  EXPECT_EQ(run.completed[1].event_id, late);
  EXPECT_EQ(run.completed[1].service_cycle, 100u);
}

TEST(EventEngine, DependencyCanDelayArrivalWithoutHoldingAResource) {
  EventEngine engine;
  const ResourceId core = engine.AddResource({"core", 1});
  const ResourceId dma = engine.AddResource({"dma", 1});
  const EventId sync = engine.Submit({"sync", core, 0, 0, {.ready_cycle = 5}});
  When after_sync;
  after_sync.after_delay.push_back({sync, 3});
  const EventId next = engine.Submit({"next", dma, 0, 2, after_sync});
  RecordingSink trace;
  const RunResult run = RunChecked(engine, &trace);

  ASSERT_EQ(run.completed.size(), 2u);
  EXPECT_EQ(engine.CompletionOf(next).value(), 10u);
  ASSERT_EQ(trace.records.size(), 2u);
  EXPECT_EQ(trace.records[1].arrival, 8u);
}

TEST(EventEngine, DelayedDependencyNeedsRememberedCompletion) {
  EventEngine engine;
  const ResourceId core = engine.AddResource({"core", 1});
  const EventId first = engine.Submit({"first", core, 0, 1, {}});
  engine.RunUntil(2);
  When delayed;
  delayed.after_delay.push_back({first, 3});
  EXPECT_FALSE(engine.Accepts(delayed));
}

TEST(EventEngine, BoundAccessSharesAHostOperationResource) {
  EventEngine engine;
  const ResourceId fabric = engine.AddResource({"FABRIC", 1});
  MainMemory<"FABRIC"> refill{3};
  engine.BindAccessNode(refill, fabric);
  engine.Submit({"DMA", fabric, 0, 5, {}});
  engine.SubmitAccess(refill, {.addr = 0x1000, .initiator_id = 1});
  RecordingSink trace;
  RunChecked(engine, &trace);
  ASSERT_EQ(trace.records.size(), 2u);
  EXPECT_EQ(trace.records[1].resource, "FABRIC");
  EXPECT_EQ(trace.records[1].arrival, 0u);
  EXPECT_EQ(trace.records[1].start, 5u);
  EXPECT_EQ(trace.records[1].finish, 8u);
}

TEST(EventEngine, CacheHintDoesNotTouchReplacementAndFenceInvalidates) {
  MainMemory<"memory"> memory{16};
  Cache<"icache", 1, 2, 512, LRUPolicy, 1, true> cache(&memory);
  const auto access = [&](uint64_t addr, AccessType type) {
    const Request request{.addr = addr, .type = type, .initiator_id = 0};
    const Route route = cache.Serve(request);
    cache.Complete(request, route.next != nullptr);
    return route.next == nullptr;
  };
  EXPECT_FALSE(access(0x1000, AccessType::kLoad));
  EXPECT_FALSE(access(0x1200, AccessType::kLoad));
  EXPECT_TRUE(access(0x1000, AccessType::kHint));
  EXPECT_FALSE(access(0x1400, AccessType::kLoad));
  EXPECT_FALSE(access(0x1000, AccessType::kLoad));
  EXPECT_TRUE(access(0, AccessType::kFence));
  EXPECT_FALSE(access(0x1000, AccessType::kLoad));
}

TEST(EventEngine, FenceCannotDiscardDirtyDataCacheLine) {
  MainMemory<"memory"> memory{16};
  Cache<"data", 1, 1, 512> cache(&memory);
  const Request store{
      .addr = 0x1000, .type = AccessType::kStore, .initiator_id = 0};
  const Route miss = cache.Serve(store);
  ASSERT_NE(miss.next, nullptr);
  cache.Complete(store, true);
  cache.Serve({.addr = 0, .type = AccessType::kFence, .initiator_id = 0});
  EXPECT_EQ(
      cache
          .Serve({.addr = 0x1000, .type = AccessType::kLoad, .initiator_id = 0})
          .next,
      nullptr);
}

TEST(EventEngine, CapacityServesThatManyAtOnce) {
  EventEngine engine;
  const ResourceId dma = engine.AddResource({"dma", 2});
  const EventId a = engine.Submit({"a", dma, 0, 10, {0, {}}});
  const EventId b = engine.Submit({"b", dma, 0, 10, {0, {}}});
  const EventId c = engine.Submit({"c", dma, 0, 10, {0, {}}});
  CheckingSink checker({{"dma", 2}});  // the checker cannot see a resource's
                                       // own declared capacity, only its name
  RunChecked(engine, checker);
  ReportViolations(checker);
  EXPECT_EQ(engine.CompletionOf(a).value(), 10u);
  EXPECT_EQ(engine.CompletionOf(b).value(), 10u);
  EXPECT_EQ(engine.CompletionOf(c).value(), 20u);
}

TEST(EventEngine, AnOperationAcquiresAllResourcesAndCompletesOnce) {
  EventEngine engine;
  const ResourceId a = engine.AddResource({"a", 1});
  const ResourceId b = engine.AddResource({"b", 1});
  const ResourceId c = engine.AddResource({"c", 1});
  engine.Submit({"occupied", b, 0, 7, {0, {}}});
  const EventId joined = engine.Submit({"joined", a, 0, 3, {1, {}}, {b, c}});
  RecordingSink trace;
  const RunResult run = RunChecked(engine, &trace);

  ASSERT_EQ(run.completed.size(), 2u);
  EXPECT_EQ(engine.CompletionOf(joined).value(), 10u);
  std::vector<std::string> held;
  for (const Record& record : trace.records) {
    if (record.op == joined) {
      held.emplace_back(record.resource);
      EXPECT_EQ(record.arrival, 1u);
      EXPECT_EQ(record.start, 7u);
      EXPECT_EQ(record.finish, 10u);
    }
  }
  EXPECT_EQ(held, (std::vector<std::string>{"a", "b", "c"}));
}

TEST(EventEngine, OverlappingResourceSetsWaitInArrivalOrder) {
  EventEngine engine;
  const ResourceId a = engine.AddResource({"a", 1});
  const ResourceId b = engine.AddResource({"b", 1});
  const ResourceId c = engine.AddResource({"c", 1});
  const ResourceId independent = engine.AddResource({"independent", 1});
  engine.Submit({"occupied", b, 0, 10, {0, {}}});
  const EventId first = engine.Submit({"first", a, 0, 4, {1, {}}, {b}});
  const EventId second = engine.Submit({"second", c, 0, 3, {2, {}}, {a}});
  const EventId later = engine.Submit({"later", c, 0, 1, {3, {}}});
  const EventId unrelated =
      engine.Submit({"unrelated", independent, 0, 2, {2, {}}});
  const RunResult run = RunChecked(engine);

  ASSERT_EQ(run.completed.size(), 5u);
  EXPECT_EQ(engine.CompletionOf(first).value(), 14u);
  EXPECT_EQ(engine.CompletionOf(second).value(), 17u);
  EXPECT_EQ(engine.CompletionOf(later).value(), 18u);
  EXPECT_EQ(engine.CompletionOf(unrelated).value(), 4u);
}

TEST(EventEngine, AdditionalResourceCapacityAndPriorityApplyTogether) {
  EventEngine engine;
  const ResourceId a = engine.AddResource({"a", 2});
  const ResourceId b = engine.AddResource({"b", 2});
  const EventId low = engine.Submit({"low", a, 0, 3, {0, {}, 0}, {b}});
  const EventId high = engine.Submit({"high", a, 0, 5, {0, {}, 2}, {b}});
  const EventId middle = engine.Submit({"middle", b, 0, 4, {0, {}, 1}, {a}});
  CheckingSink checker({{"a", 2}, {"b", 2}});
  const RunResult run = RunChecked(engine, checker);
  ReportViolations(checker);

  ASSERT_EQ(run.completed.size(), 3u);
  EXPECT_EQ(engine.CompletionOf(high).value(), 5u);
  EXPECT_EQ(engine.CompletionOf(middle).value(), 4u);
  EXPECT_EQ(engine.CompletionOf(low).value(), 7u);
}

TEST(EventEngine, MultiResourceDependenciesAreRecordedOnce) {
  EventEngine engine;
  const ResourceId a = engine.AddResource({"a", 1});
  const ResourceId b = engine.AddResource({"b", 1});
  const EventId first = engine.Submit({"first", b, 0, 5, {0, {}}});
  const EventId second = engine.Submit({"second", a, 0, 2, {0, {first}}, {b}});
  auto count = Fold(std::size_t{0}, [second](std::size_t n, const Record& r) {
    return n + (r.op == second ? r.after.size() : 0);
  });
  const RunResult run = RunChecked(engine, &count);

  ASSERT_EQ(run.completed.size(), 2u);
  EXPECT_EQ(engine.CompletionOf(second).value(), 7u);
  EXPECT_EQ(count.value(), 1u);
}

TEST(EventEngine, AdditionalResourcesMustBeKnownAndDistinct) {
  EventEngine engine;
  const ResourceId a = engine.AddResource({"a", 1});
  const ResourceId b = engine.AddResource({"b", 1});
  EXPECT_FALSE(engine.Accepts({"unknown", a, 0, 1, {}, {99}}));
  EXPECT_FALSE(engine.Accepts({"primary_repeated", a, 0, 1, {}, {a}}));
  EXPECT_FALSE(engine.Accepts({"repeated", a, 0, 1, {}, {b, b}}));
  EXPECT_TRUE(engine.Accepts({"valid", a, 0, 1, {}, {b}}));
}

TEST(EventEngine, ZeroCostDependencyReleasesBeforeSameCycleContenders) {
  EventEngine engine;
  const ResourceId a = engine.AddResource({"a", 1});
  const ResourceId b = engine.AddResource({"b", 1});
  const ResourceId c = engine.AddResource({"c", 1});
  const EventId gate = engine.Submit({"gate", c, 0, 0, {0, {}, 3}});
  const EventId joined =
      engine.Submit({"joined", a, 0, 5, {0, {gate}, 2}, {b}});
  const EventId contender = engine.Submit({"contender", a, 0, 5, {0, {}, 1}});
  RunChecked(engine);

  EXPECT_EQ(engine.CompletionOf(joined).value(), 5u);
  EXPECT_EQ(engine.CompletionOf(contender).value(), 10u);
}

TEST(EventEngine, NodeAccessesComposeWithQueuedAtomicWorkAcrossWindows) {
  MainMemory<"memory"> memory(3);
  Cache<"cache", 1, 2, 64, LRUPolicy, 1> cache(&memory);
  EventEngine engine;
  const ResourceId a = engine.AddResource({"a", 1});
  const ResourceId b = engine.AddResource({"b", 1});
  const EventId load = engine.SubmitAccess(cache, {.addr = 0});
  CheckingSink checker;
  RunChecked(engine, 5, checker);
  checker.Retire(5);

  engine.Submit({"occupied", b, 0, 8, {5, {}}});
  const EventId joined = engine.Submit({"joined", a, 0, 2, {5, {load}}, {b}});
  const EventId reload =
      engine.SubmitAccess(cache, {.addr = 0}, {.after = {joined}});
  const EventId independent =
      engine.SubmitAccess(cache, {.addr = 64}, {.ready_cycle = 6});
  RunChecked(engine, checker);
  ReportViolations(checker);

  EXPECT_EQ(engine.CompletionOf(independent).value(), 10u);
  EXPECT_EQ(engine.CompletionOf(joined).value(), 15u);
  EXPECT_EQ(engine.CompletionOf(reload).value(), 16u);
}

TEST(EventEngine, ADependencyMustAlreadyBeSubmitted) {
  // Dependencies name only ids this engine already returned, so the
  // dependency graph is acyclic by construction. A host that must reject a
  // bad program in a release build asks first rather than being told after.
  EventEngine engine;
  const ResourceId resource = engine.AddResource({"r", 1});
  const EventId first = engine.Submit({"first", resource, 0, 1, {0, {}}});

  EXPECT_TRUE(engine.Knows(first));
  EXPECT_FALSE(engine.Knows(99));
  EXPECT_TRUE(engine.Accepts({"ok", resource, 0, 1, {0, {first}}}));
  EXPECT_FALSE(engine.Accepts({"forward", resource, 0, 1, {0, {99}}}));
  EXPECT_FALSE(engine.Accepts({"no_resource", 99, 0, 1, {0, {}}}));
}

#ifndef NDEBUG
TEST(EventEngineDeathTest, InvalidConfigurationAsserts) {
  EventEngine engine;
  EXPECT_DEATH(engine.AddResource({"", 1}), "");
  EXPECT_DEATH(engine.AddResource({"invalid", 0}), "");
  EXPECT_DEATH(engine.Submit({"invalid", 99, 0, 1, {0, {}}}), "");
  EXPECT_DEATH(static_cast<void>(engine.CompletionOf(99)), "");
}
#endif

TEST(EventEngine, CompletionPastTheEndOfTimeSaturates) {
  EventEngine engine;
  const ResourceId resource = engine.AddResource({"overflow", 1});
  const EventId reaches_limit =
      engine.Submit({"limit", resource, 0, 1, {Cycle::Max() - 1, {}}});
  const EventId overflows =
      engine.Submit({"over", resource, 0, 1, {0, {reaches_limit}}});
  RunChecked(engine);
  EXPECT_EQ(engine.CompletionOf(reaches_limit).value(), Cycle::Max());
  EXPECT_EQ(engine.CompletionOf(overflows).value(), Cycle::Max());
}

TEST(EventEngine, OperationsAndAccessesShareOneTimeline) {
  // A host's cost table and a memory hierarchy on one timeline: a core
  // dispatches, the dispatched work loads its operand through a cache, and a
  // MAC array computes once the operand has arrived. Static costs and a
  // node's dynamic cost compose through the same dependencies.
  MainMemory<"DRAM"> dram(100);
  Cache<"L1", 1, 2, 64, LRUPolicy, 4> l1(&dram);
  EventEngine engine;
  const ResourceId core = engine.AddResource({"core", 1});
  const ResourceId mac = engine.AddResource({"mac", 1});

  const EventId dispatch = engine.Submit({"dispatch", core, 0, 1, {0, {}}});
  const EventId load = engine.SubmitAccess(
      l1, {.addr = 0x0000, .type = AccessType::kLoad}, {.after = {dispatch}});
  const EventId matmul = engine.Submit({"matmul", mac, 0, 256, {0, {load}}});
  // The same operand again: a hit now, so only the lookup is paid.
  const EventId reload = engine.SubmitAccess(
      l1, {.addr = 0x0000, .type = AccessType::kLoad}, {.after = {matmul}});
  const RunResult run = RunChecked(engine);

  EXPECT_EQ(engine.CompletionOf(dispatch).value(), 1u);
  EXPECT_EQ(engine.CompletionOf(load).value(),
            105u);  // 1 + lookup 4 + DRAM 100
  EXPECT_EQ(engine.CompletionOf(matmul).value(), 361u);
  EXPECT_EQ(engine.CompletionOf(reload).value(), 365u);

  // Each access reports where it was served.
  for (const auto& result : run.completed) {
    if (result.event_id == load) EXPECT_EQ(result.served_by, "DRAM");
    if (result.event_id == reload) EXPECT_EQ(result.served_by, "L1");
    if (result.event_id == matmul) EXPECT_EQ(result.served_by, "mac");
  }
}

TEST(EventEngine, CompletionFillsCacheBeforeAnArrivalAtTheSameCycle) {
  MainMemory<"DRAM"> dram(100);
  Cache<"L1", 1, 1, 64, LRUPolicy, 1> l1(&dram);
  EventEngine engine;

  const EventId first = engine.SubmitAccess(l1, {.addr = 0x0000});
  const EventId second =
      engine.SubmitAccess(l1, {.addr = 0x0000}, {.ready_cycle = 101});
  RecordingSink records;
  engine.RunUntilIdle(&records);

  EXPECT_EQ(engine.CompletionOf(second).value(), 102u);
  ASSERT_EQ(records.records.size(), 3u);
  EXPECT_EQ(records.records[1].op, first);
  EXPECT_EQ(records.records[1].finish, 101u);
  EXPECT_EQ(records.records[2].op, second);
  EXPECT_EQ(records.records[2].start, 101u);
}

// Three agents sharing a memory behind private caches, each issuing at fixed
// cycles, plus a stream of fixed-cost operations. Submitted in windows of
// `window` cycles (0: all at once), run window by window. Returns every
// record, one line each.
static std::vector<std::string> SharedMemoryRun(Cycle window) {
  MainMemory<"DRAM"> dram(100);
  Cache<"L2", 16, 4, 64, LRUPolicy, 10> l2(&dram);
  Cache<"L1a", 4, 2, 64, LRUPolicy, 2> l1a(&l2);
  Cache<"L1b", 4, 2, 64, LRUPolicy, 2> l1b(&l2);
  Cache<"L1c", 4, 2, 64, LRUPolicy, 2> l1c(&l2);
  AccessNode* entry[] = {&l1a, &l1b, &l1c};
  EventEngine engine;
  const ResourceId mac = engine.AddResource({"mac", 1});
  RecordingSink trace;
  CheckingSink checker;
  BroadcastSink both(&trace, &checker);

  constexpr Cycle kEnd = 20000;
  Cycle submitted_to = 0;
  Cycle t = 0;  // the next issue cycle, continuing across windows
  while (submitted_to < kEnd) {
    const Cycle until =
        window == 0 ? kEnd : std::min(kEnd, submitted_to + window);
    for (; t < until; t += 7) {
      const auto agent = static_cast<uint32_t>(t.value() % 3);
      const uint64_t addr = ((t.value() * 2654435761ULL) >> 4) % 4096 * 64;
      const AccessType type =
          t % 5 == 0 ? AccessType::kStore : AccessType::kLoad;
      engine.SubmitAccess(*entry[agent],
                          {.addr = addr, .type = type, .initiator_id = agent},
                          {.ready_cycle = t});
      if (t % 91 == 0) engine.Submit({"matmul", mac, 0, 40, {t, {}}});
    }
    submitted_to = until;
    if (window == 0) break;
    engine.RunUntil(submitted_to, &both);
    checker.Retire(submitted_to);
  }
  engine.RunUntilIdle(&both);
  for (const auto& v : checker.Violations()) ADD_FAILURE() << v.what;

  std::vector<std::string> lines;
  for (const Record& r : trace.records) {
    lines.push_back(std::to_string(r.op.value()) + " " +
                    std::string(r.resource) + " " + std::to_string(r.addr) +
                    " " + std::to_string(r.arrival.value()) + " " +
                    std::to_string(r.start.value()) + " " +
                    std::to_string(r.finish.value()) + " " +
                    std::to_string(r.depth) + (r.forwarded ? " fwd" : ""));
  }
  return lines;
}

// The first record where two runs part ways, or "" if they never do.
static std::string FirstDifference(const std::vector<std::string>& a,
                                   const std::vector<std::string>& b) {
  for (std::size_t i = 0; i < std::min(a.size(), b.size()); ++i) {
    if (a[i] != b[i]) {
      return "record " + std::to_string(i) + ": " + a[i] + " vs " + b[i];
    }
  }
  return a.size() == b.size() ? "" : "lengths differ";
}

TEST(EventEngine, RunningInWindowsMatchesRunningAllAtOnce) {
  // A host that submits work up to a cycle and runs to it gets exactly what
  // one that submits everything first gets: every record, in order. Several
  // agents contend at the shared levels, so a window that let a later arrival
  // be served early would show here.
  const auto all_at_once = SharedMemoryRun(0);
  ASSERT_GT(all_at_once.size(), 5000u);
  EXPECT_EQ(FirstDifference(SharedMemoryRun(500), all_at_once), "");
  EXPECT_EQ(FirstDifference(SharedMemoryRun(37), all_at_once), "");
}

TEST(EventEngine, AtomicResourceSetsHaveIdenticalRecordsAcrossWindows) {
  const auto replay = [](Cycle width) {
    EventEngine engine;
    const ResourceId a = engine.AddResource({"a", 2});
    const ResourceId b = engine.AddResource({"b", 1});
    const ResourceId c = engine.AddResource({"c", 2});
    RecordingSink trace;
    CheckingSink checker({{"a", 2}, {"b", 1}, {"c", 2}});
    BroadcastSink both(&trace, &checker);
    EventId previous = 0;
    for (uint64_t t = 0; t < 200; ++t) {
      std::vector<EventId> after;
      if (previous != 0 && t % 3 == 0) {
        after.push_back(previous);
      }
      previous = engine.Submit({"work",
                                a,
                                0,
                                t % 5,
                                {t, std::move(after), uint32_t(t % 3)},
                                t % 2 == 0 ? std::vector<ResourceId>{b, c}
                                           : std::vector<ResourceId>{c}});
      if (width != 0 && (t + 1) % width.value() == 0) {
        engine.RunUntil(t + 1, &both);
        checker.Retire(t + 1);
      }
    }
    engine.RunUntilIdle(&both);
    ReportViolations(checker);
    std::vector<std::string> lines;
    for (const Record& r : trace.records) {
      lines.push_back(std::to_string(r.op.value()) + " " +
                      std::string(r.resource) + " " +
                      std::to_string(r.arrival.value()) + " " +
                      std::to_string(r.start.value()) + " " +
                      std::to_string(r.finish.value()));
    }
    return lines;
  };
  const auto all = replay(0);
  EXPECT_EQ(all.size(), 500u);
  EXPECT_EQ(FirstDifference(replay(5), all), "");
  EXPECT_EQ(FirstDifference(replay(37), all), "");
}

TEST(EventEngine, CompletionIsPendingUntilItRunsAndRetiredPastTheHorizon) {
  EventEngine engine;
  const ResourceId r = engine.AddResource({"r", 1});
  const EventId id = engine.Submit({"op", r, 0, 10, {0, {}}});
  const auto pending = engine.CompletionOf(id);
  EXPECT_FALSE(pending.has_value());
  EXPECT_EQ(pending.error(), Absent::kPending);

  RunChecked(engine);
  EXPECT_EQ(engine.CompletionOf(id).value(), 10u);

  RunChecked(engine, 100);
  const auto retired = engine.CompletionOf(id);
  EXPECT_FALSE(retired.has_value());
  EXPECT_EQ(retired.error(), Absent::kRetired);
}

TEST(EventEngine, WorkBeforeTheHorizonIsRetired) {
  // Once the engine has run past a cycle, what completed before it is
  // forgotten: depending on it is already satisfied, since nothing submitted
  // afterwards may begin before that cycle anyway.
  EventEngine engine;
  const ResourceId r = engine.AddResource({"r", 1});
  const EventId early = engine.Submit({"early", r, 0, 10, {0, {}}});
  CheckingSink checker;  // kept across both runs, and retired the same cycle
                         // the engine is, so "later" depending on the by-then-
                         // retired "early" is not mistaken for a real bug
  RunChecked(engine, 100, checker);
  checker.Retire(100);
  EXPECT_TRUE(engine.Knows(early));
  EXPECT_EQ(engine.CompletionOf(early).error(), Absent::kRetired);

  const EventId later = engine.Submit({"later", r, 0, 5, {100, {early}}});
  RunChecked(engine, checker);
  EXPECT_EQ(engine.CompletionOf(later).value(), 105u);
  ReportViolations(checker);
}

TEST(EventEngine, NothingMayBeSubmittedBeforeTheHorizon) {
  // The window is a promise: once run to a cycle, the host submits nothing
  // that could begin earlier. A release build asks before submitting.
  EventEngine engine;
  const ResourceId r = engine.AddResource({"r", 1});
  RunChecked(engine, 100);
  EXPECT_FALSE(engine.Accepts({"too_early", r, 0, 1, {50, {}}}));
  EXPECT_TRUE(engine.Accepts({"on_time", r, 0, 1, {100, {}}}));

  // An access is checked the same way, by when it may begin.
  EXPECT_FALSE(engine.Accepts(When{.ready_cycle = 50}));
  EXPECT_TRUE(engine.Accepts(When{.ready_cycle = 100}));
  EXPECT_FALSE(engine.Accepts(When{.ready_cycle = 100, .after = {99}}));
}

// The most the engine held at once over a run of `windows` windows of 1000
// cycles, 20 accesses each. The working set fits the cache and accesses are
// spaced so the memory keeps up with the cold misses.
static std::size_t PeakInFlight(std::uint64_t windows) {
  MainMemory<"DRAM"> dram(100);
  Cache<"L1", 64, 4, 64, LRUPolicy, 2> l1(&dram);
  EventEngine engine;
  CheckingSink checker;
  std::size_t peak = 0;
  for (std::uint64_t w = 0; w < windows; ++w) {
    for (std::uint64_t t = w * 1000; t < (w + 1) * 1000; t += 50) {
      engine.SubmitAccess(l1, {.addr = (t * 64) % (128 * 64)},
                          {.ready_cycle = t});
    }
    const Cycle horizon = (w + 1) * 1000;
    engine.RunUntil(horizon, &checker);
    checker.Retire(horizon);
    peak = std::max(peak, engine.InFlight());
  }
  for (const auto& v : checker.Violations()) ADD_FAILURE() << v.what;
  return peak;
}

TEST(EventEngine, InFlightWorkStaysBoundedAcrossWindows) {
  // What the engine holds is what is in flight, not everything ever
  // submitted: twice the run holds no more.
  const std::size_t peak = PeakInFlight(200);  // 4,000 accesses
  EXPECT_EQ(PeakInFlight(400), peak);          // 8,000 accesses
  EXPECT_LT(peak, 100u);
}
