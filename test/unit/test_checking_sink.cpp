// Copyright 2025-2026 Yi-Ping Pan (Cloudlet)

#include <vector>

#include "gtest/gtest.h"
#include "timeball/checking_sink.hpp"
#include "timeball/timeball.hpp"

using namespace timeball;

namespace {

// A Record with every field given explicitly, so a test reads as the shape of
// the violation it is aimed at rather than a builder's defaults.
Record Rec(EventId op, std::string_view resource, Cycle arrival, Cycle start,
           Cycle finish, std::vector<EventId>& deps_storage) {
  return Record{.op = op,
                .name = "op",
                .resource = resource,
                .arrival = arrival,
                .start = start,
                .finish = finish,
                .after = deps_storage};
}

}  // namespace

TEST(CheckingSink, PassesOnAWellFormedRun) {
  // A real run, through real nodes and a mix of static operations and memory
  // accesses: the checker must raise nothing against the engine's own output.
  MainMemory<"DRAM"> dram(100);
  Cache<"L1", 4, 2, 64, LRUPolicy, 2> l1(&dram);
  EventEngine engine;
  const ResourceId mac = engine.AddResource({"mac", 1});
  Initiator core(engine, 0);

  const EventId load = core.Issue(l1, 0x0000, AccessType::kLoad);
  engine.Submit({"matmul", mac, 0, 40, {0, {load}}});
  core.Issue(l1, 0x0040, AccessType::kLoad);
  core.Issue(l1, 0x0000, AccessType::kLoad);  // now a hit

  CheckingSink checker;
  engine.RunUntilIdle(&checker);
  EXPECT_TRUE(checker.Ok());
  for (const auto& v : checker.Violations()) {
    ADD_FAILURE() << v.what;
  }
}

TEST(CheckingSink, PassesOnManyAgentsSharingCachesAndAWindowedRun) {
  // The scenario several agents contending at shared levels, run once straight
  // through and once in narrow windows: the richest case the engine handles,
  // and the one a real defect would most likely show up in.
  MainMemory<"DRAM"> dram(100);
  Cache<"L2", 16, 4, 64, LRUPolicy, 10> l2(&dram);
  Cache<"L1a", 4, 2, 64, LRUPolicy, 2> l1a(&l2);
  Cache<"L1b", 4, 2, 64, LRUPolicy, 2> l1b(&l2);
  AccessNode* entry[] = {&l1a, &l1b};
  EventEngine engine;
  CheckingSink checker;

  for (Cycle t = 0; t < 2000; t += 7) {
    const auto agent = static_cast<uint32_t>(t.value() % 2);
    engine.SubmitAccess(*entry[agent],
                        {.addr = ((t.value() * 2654435761ULL) >> 4) % 512 * 64,
                         .initiator_id = agent},
                        {.ready_cycle = t});
    if (t.value() % 300 == 0) {
      engine.RunUntil(t, &checker);
      checker.Retire(t);
    }
  }
  engine.RunUntilIdle(&checker);
  EXPECT_TRUE(checker.Ok());
  for (const auto& v : checker.Violations()) {
    ADD_FAILURE() << v.what;
  }
}

TEST(CheckingSink, CatchesArrivalAfterStartOrStartAfterFinish) {
  CheckingSink checker;
  std::vector<EventId> none;
  checker.OnRecord(
      Rec(1, "r", /*arrival=*/10, /*start=*/5, /*finish=*/20, none));
  EXPECT_FALSE(checker.Ok());
}

TEST(CheckingSink, CatchesADependencyNotYetComplete) {
  CheckingSink checker;
  std::vector<EventId> none;
  std::vector<EventId> dep_on_1{1};

  // Op 1 finishes at 5.
  checker.OnRecord(Rec(1, "a", 0, 0, 5, none));
  // Op 2 claims to depend on 1 but arrives at 3, before 1 finished.
  checker.OnRecord(Rec(2, "b", 3, 3, 4, dep_on_1));
  EXPECT_FALSE(checker.Ok());
}

TEST(CheckingSink, CatchesADependencyThatWasNeverSubmitted) {
  CheckingSink checker;
  std::vector<EventId> dep_on_99{99};
  checker.OnRecord(Rec(1, "a", 10, 10, 11, dep_on_99));
  EXPECT_FALSE(checker.Ok());
}

TEST(CheckingSink, CatchesTwoRecordsExceedingTheirResourcesCapacity) {
  // Capacity 1, but two records overlap: [1,6) and [0,7).
  CheckingSink checker({{"r", 1}});
  std::vector<EventId> none;
  checker.OnRecord(Rec(2, "r", 1, 1, 6, none));
  checker.OnRecord(Rec(1, "r", 0, 0, 7, none));
  EXPECT_FALSE(checker.Ok());
}

TEST(CheckingSink, AllowsConcurrentUseUpToCapacity) {
  // The same overlap as above, but capacity 2: no violation.
  CheckingSink checker({{"r", 2}});
  std::vector<EventId> none;
  checker.OnRecord(Rec(2, "r", 1, 1, 6, none));
  checker.OnRecord(Rec(1, "r", 0, 0, 7, none));
  EXPECT_TRUE(checker.Ok());
}

TEST(CheckingSink, CatchesAnEarlierArrivalStartedAfterALaterOne) {
  // B arrives at 1, starts at 2. A arrives at 0 (earlier) but starts at 5
  // (later than B) — served out of arrival order. Capacity 2 so this is
  // isolated from the capacity check above.
  CheckingSink checker({{"r", 2}});
  std::vector<EventId> none;
  checker.OnRecord(Rec(2, "r", /*arrival=*/1, /*start=*/2, /*finish=*/6, none));
  checker.OnRecord(Rec(1, "r", /*arrival=*/0, /*start=*/5, /*finish=*/7, none));
  EXPECT_FALSE(checker.Ok());
}

TEST(CheckingSink,
     CatchesArrivalOrderViolationEvenLongAfterTheSkippedOpFinished) {
  // B arrives at 1, is served (wrongly) first: starts at 2, finishes at 3. A
  // arrives at 0 — earlier than B — but only starts at 10, long after B has
  // already finished and stopped overlapping it in time. The capacity check
  // alone would see no overlap and say nothing; arrival order must still be
  // checked against B even though it is no longer "active".
  CheckingSink checker;
  std::vector<EventId> none;
  checker.OnRecord(Rec(2, "r", /*arrival=*/1, /*start=*/2, /*finish=*/3, none));
  checker.OnRecord(
      Rec(1, "r", /*arrival=*/0, /*start=*/10, /*finish=*/11, none));
  EXPECT_FALSE(checker.Ok());
}

TEST(CheckingSink, RetireForgetsOldCompletionsWithoutFalsePositives) {
  CheckingSink checker;
  std::vector<EventId> none;
  checker.OnRecord(Rec(1, "a", 0, 0, 5, none));
  checker.Retire(6);  // strictly past op 1's finish, so it is truly forgotten

  // A dependency on the now-forgotten op 1 is no longer checkable and must not
  // be reported as a violation: retiring must not turn into false positives on
  // ordinary windowed use.
  std::vector<EventId> dep_on_1{1};
  checker.OnRecord(Rec(2, "b", 10, 10, 11, dep_on_1));
  EXPECT_TRUE(checker.Ok());
}

TEST(CheckingSink, StillCatchesADependencyViolationBeforeAnythingIsRetired) {
  // Retire() trades detection power for a bounded memory; before it is ever
  // called there is nothing to be ambiguous about, so a missing dependency is
  // still exactly what it looks like: a real violation.
  CheckingSink checker;
  std::vector<EventId> dep_on_99{99};
  checker.OnRecord(Rec(1, "a", 10, 10, 11, dep_on_99));
  EXPECT_FALSE(checker.Ok());
}
