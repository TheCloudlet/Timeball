// Copyright 2025-2026 Yi-Ping Pan (Cloudlet)

#include <cstdio>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "checked_run.hpp"
#include "gtest/gtest.h"
#include "timeball/checking_sink.hpp"
#include "timeball/timeball.hpp"

#ifdef TIMEBALL_WITH_SQLITE
#include <sqlite3.h>

#include "timeball/event_store.hpp"

// Reads one integer back out of the database, so the test asserts against the
// file a run left behind rather than against the engine's own memory.
static int SqliteScalar(const std::string& path, const char* sql) {
  sqlite3* db = nullptr;
  if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) return -1;
  sqlite3_stmt* st = nullptr;
  int out = -1;
  if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK &&
      sqlite3_step(st) == SQLITE_ROW) {
    out = sqlite3_column_int(st, 0);
  }
  sqlite3_finalize(st);
  sqlite3_close(db);
  return out;
}
#endif

using namespace timeball;
using namespace timeball::test;

// Tiny Cache for testing evictions
using Mem = MainMemory<"MainMemory">;
using TinyCache = Cache<"Tiny", 1, 2, 64, LRUPolicy, 1>;

// Where an access was served, and when it completed.
struct AccessResult {
  std::string_view hit_level;
  Cycle complete_cycle = 0;
};

// One access: its address, its type, and the cycle it may begin.
struct Timed {
  uint64_t addr = 0;
  AccessType type = AccessType::kLoad;
  Cycle at = 0;
};

// Times accesses one at a time: submit one, run the engine until idle, report
// where it was served and when it completed. Sound only because every test
// using it issues in time order — with several agents, submitting and running
// one access at a time would serve them in call order. Every record the engine
// produced is kept in trace.
struct OneAtATime {
  EventEngine engine;
  RecordingSink trace;
  CheckingSink checker;

  // The checker names a resource by the node's declared name (Record has no
  // per-instance identity), so two distinct node instances sharing one name —
  // as two same-typed caches necessarily do — are indistinguishable to it and
  // merge into one bucket. A test that constructs several such instances gives
  // their combined true capacity here, e.g. {{"Tiny", 2}} for two capacity-1
  // caches both named "Tiny"; this restores the capacity check and leaves the
  // others (order, dependency) intact.
  explicit OneAtATime(
      std::unordered_map<std::string, std::uint32_t> capacities = {})
      : checker(std::move(capacities)) {}

  ~OneAtATime() {
    for (const auto& v : checker.Violations()) {
      ADD_FAILURE() << v.what;
    }
  }

  AccessResult Access(AccessNode& node, Timed t) {
    const EventId id = engine.SubmitAccess(
        node, {.addr = t.addr, .type = t.type}, {.ready_cycle = t.at});
    BroadcastSink both(&trace, &checker);
    const RunResult run = engine.RunUntilIdle(&both);
    for (const auto& result : run.completed) {
      if (result.event_id == id) {
        return {result.served_by, result.completion_cycle};
      }
    }
    ADD_FAILURE() << "access " << id.value() << " did not complete";
    return {};
  }
};

TEST(Cache, EvictionHitMissAndStoreSequence) {
  OneAtATime tl;
  // Nodes are constructed separately and wired together.
  Mem mem(100);
  TinyCache tiny(&mem);
  AccessNode* cache = &tiny;

  auto load = [&](uint64_t addr, uint64_t at) {
    return tl.Access(*cache, {addr, AccessType::kLoad, at});
  };

  // Fill Set 0
  load(0x0000, 0);  // Way 0
  load(0x0040, 0);  // Way 1 (Set 0 is full now)

  // Access Way 0 again to make it MRU
  load(0x0000, 0);

  // Load new block -> Should evict Way 1 (0x0040)
  auto res = load(0x0080, 0);
  EXPECT_EQ(res.hit_level, "MainMemory");

  // A hit completes one cycle after it was issued, wherever on the timeline it
  // was issued — the returned cycle is absolute, not a latency.
  auto hit = load(0x0080, 5000);
  EXPECT_EQ(hit.hit_level, "Tiny");
  EXPECT_EQ(hit.complete_cycle, 5001u);

  // A miss costs this level's lookup plus the level below: 1 + 100.
  auto miss = load(0x4000, 7000);
  EXPECT_EQ(miss.hit_level, "MainMemory");
  EXPECT_EQ(miss.complete_cycle, 7101u);

  // A store hit marks the line dirty without reaching the level below.
  auto store = tl.Access(*cache, {0x4000, AccessType::kStore, 9000});
  EXPECT_EQ(store.hit_level, "Tiny");
  EXPECT_EQ(store.complete_cycle, 9001u);
}

TEST(Cache, SharedSuccessorContentionAndPerNodeState) {
  // Two distinct "Tiny" instances: their combined true capacity is 2, given
  // explicitly since the checker cannot see instance identity (see OneAtATime).
  OneAtATime tl({{"Tiny", 2}});
  // Two caches referencing one successor — the thing a template-bound successor
  // cannot express, since each would need a distinct type. Asserted through
  // completion cycles rather than by inspecting the edge.
  Mem shared(100);
  TinyCache a(&shared);
  TinyCache b(&shared);

  // Each cache holds its own lines, so both miss and both reach the one memory.
  // b missing proves a's fill did not populate b.
  //
  // Both issue at cycle 0, so they contend: a's lookup ends at 1 and holds the
  // memory from 1 to 101; b's lookup also ends at 1, but the memory is busy
  // until 101, so b is served 101..201. This is the contention that a scalar
  // latency cannot express — without occupancy both would report 101.
  auto ra = tl.Access(a, {0x0000, AccessType::kLoad, 0});
  auto rb = tl.Access(b, {0x0000, AccessType::kLoad, 0});
  EXPECT_EQ(ra.hit_level, "MainMemory");
  EXPECT_EQ(ra.complete_cycle, 101u);
  EXPECT_EQ(rb.hit_level, "MainMemory");
  EXPECT_EQ(rb.complete_cycle, 201u);

  // And the shared memory really is one node reached by both: a hit in a
  // afterwards stays local, confirming a filled its own line.
  auto again = tl.Access(a, {0x0000, AccessType::kLoad, 500});
  EXPECT_EQ(again.hit_level, "Tiny");
  EXPECT_EQ(again.complete_cycle, 501u);
}

TEST(Cache, FreeNodeImposesNoWait) {
  OneAtATime tl;
  // An access arriving after the node has freed up does not wait. The shared
  // memory is busy until 201; an access issued at 5000 is served immediately.
  Mem late_mem(100);
  TinyCache late(&late_mem);
  tl.Access(late, {0x0000, AccessType::kLoad, 0});  // holds memory 1..101
  auto unhindered = tl.Access(late, {0x8000, AccessType::kLoad, 5000});
  EXPECT_EQ(unhindered.hit_level, "MainMemory");
  EXPECT_EQ(unhindered.complete_cycle, 5101u);
}

TEST(Cache, InOrderInitiatorNeverWaitsOnItself) {
  OneAtATime tl;
  // A single in-order initiator never contends with itself: it issues the next
  // access only once the previous completed, so the node is always free and
  // every completion is plain latency, with no wait added.
  Mem solo_mem(100);
  TinyCache solo(&solo_mem);
  Cycle now = 0;
  // miss(+101), miss(+101), hit(+1), miss evicting LRU 0x0040(+101), hit on
  // 0x0000, which survived the eviction(+1).
  const uint64_t addrs[] = {0x0000, 0x0040, 0x0000, 0x0080, 0x0000};
  const uint64_t want[] = {101, 202, 203, 304, 305};
  for (size_t i = 0; i < 5; ++i) {
    now = tl.Access(solo, {addrs[i], AccessType::kLoad, now}).complete_cycle;
    EXPECT_EQ(now, want[i]) << "access index " << i;
  }
}

TEST(Cache, ThreeLevelWritebackLeavesRequesterPathAlone) {
  OneAtATime tl;
  // Through three levels, the requester's own completion is still the demand
  // path alone. The writeback's cost lands on the level below, which the
  // two-level test observes directly.
  MainMemory<"WB3"> wb_mem(100);
  Cache<"WB2", 1, 1, 64, LRUPolicy, 10> wb2(&wb_mem);
  Cache<"WB1", 1, 1, 64, LRUPolicy, 1> wb1(&wb2);
  Cycle t = tl.Access(wb1, {0x0000, AccessType::kStore, 0}).complete_cycle;
  ASSERT_EQ(t, 111u);

  // Evicts the dirty line; the demand path is 111 + 1 + 10 + 100 = 222.
  t = tl.Access(wb1, {0x4000, AccessType::kLoad, t}).complete_cycle;
  EXPECT_EQ(t, 222u);
}

TEST(Cache, EngineRecordsEveryHopOfAnAccess) {
  OneAtATime tl;
  // Nodes record nothing: the engine records each resource an access occupies,
  // so a miss here leaves one record per node it visited and a hit leaves one.
  MainMemory<"PMem"> pmem(100);
  Cache<"PC", 1, 2, 64, LRUPolicy, 1> pc(&pmem);

  tl.Access(pc, {0x0000, AccessType::kLoad, 0});    // miss
  tl.Access(pc, {0x0000, AccessType::kLoad, 500});  // hit

  const auto& records = tl.trace.records;
  ASSERT_EQ(records.size(), 3u);
  EXPECT_EQ(records[0].resource, "PC");
  EXPECT_TRUE(records[0].forwarded);  // the miss went on
  EXPECT_EQ(records[1].resource, "PMem");
  EXPECT_FALSE(records[1].forwarded);
  EXPECT_EQ(records[2].resource, "PC");
  EXPECT_FALSE(records[2].forwarded);  // the hit stopped here
  EXPECT_EQ(records[2].arrival, 500u);
  EXPECT_EQ(records[2].finish, 501u);
}

TEST(Cache, RecordsSplitWaitingFromWorking) {
  // See SharedSuccessorContentionAndPerNodeState above: two "Tiny" instances,
  // combined capacity given explicitly.
  OneAtATime tl({{"Tiny", 2}});
  // Every record says how long its resource made it wait and how long it
  // worked, so a report can tell a slow node from a busy one. Two caches share
  // a memory: the second miss arrives while the memory is busy.
  Mem shared(100);
  TinyCache a(&shared);
  TinyCache b(&shared);
  tl.Access(a, {0x0000, AccessType::kLoad, 0});
  tl.Access(b, {0x0000, AccessType::kLoad, 0});

  const Record* waited = nullptr;
  for (const auto& r : tl.trace.records) {
    if (r.resource == "MainMemory" && r.op == 2) waited = &r;
  }
  ASSERT_NE(waited, nullptr);
  EXPECT_EQ(waited->arrival, 1u);
  EXPECT_EQ(waited->start - waited->arrival, 100u);  // waiting
  EXPECT_EQ(waited->finish - waited->start, 100u);   // working
}

TEST(Cache, WritebacksRecordedButNonDemand) {
  OneAtATime tl;
  // Writebacks are recorded but belong to no operation: they hold the node they
  // are sent to, yet no initiator waited on them, so per-requester statistics
  // exclude them.
  MainMemory<"EMem"> emem(100);
  Cache<"E1", 1, 1, 64, LRUPolicy, 4> e1(&emem);
  Cycle at = 0;
  for (uint64_t addr : {0x0000ULL, 0x4000ULL}) {
    at = tl.Access(e1, {addr, AccessType::kStore, at}).complete_cycle;
  }
  size_t demand = 0, writeback = 0;
  for (const auto& r : tl.trace.records) {
    if (r.demand()) {
      ++demand;
    } else {
      ++writeback;
    }
  }
  EXPECT_GT(demand, 0u);
  EXPECT_GT(writeback, 0u);
}

TEST(Cache, RecordsCarryNestingDepthNotCycleOrder) {
  OneAtATime tl;
  // Depth is recorded, not inferred from cycles: with a zero-latency middle
  // level, two hops share a cycle and order alone cannot tell nesting from
  // succession.
  MainMemory<"DMem"> dmem(100);
  Cache<"D2", 4, 2, 64, LRUPolicy, 0> d2(&dmem);
  Cache<"D1", 2, 2, 64, LRUPolicy, 4> d1(&d2);
  tl.Access(d1, {0x1000, AccessType::kLoad, 0});

  // D1 costs 4, D2 costs 0, memory 100: the memory worked 100 of the 104.
  const auto& records = tl.trace.records;
  ASSERT_EQ(records.size(), 3u);
  EXPECT_EQ(records[0].resource, "D1");
  EXPECT_EQ(records[0].depth, 0u);
  EXPECT_EQ(records[1].resource, "D2");
  EXPECT_EQ(records[1].depth, 1u);
  EXPECT_EQ(records[1].start, records[1].finish);  // zero latency
  EXPECT_EQ(records[2].resource, "DMem");
  EXPECT_EQ(records[2].depth, 2u);
  EXPECT_EQ(records[2].finish - records[2].start, 100u);
  EXPECT_EQ(records[2].finish, 104u);
}

TEST(Cache, WritebackRecordsTheDepthItForwardedFrom) {
  OneAtATime tl;
  // A writeback sent from a cache sits one level below that cache — the same
  // depth a demand forward from it reaches — not at depth 0.
  MainMemory<"DWMem"> dwmem(100);
  Cache<"DW2", 1, 1, 64, LRUPolicy, 1> dw2(&dwmem);
  Cache<"DW1", 1, 1, 64, LRUPolicy, 1> dw1(&dw2);

  // Dirty the only line in dw1.
  Cycle t = tl.Access(dw1, {0x0000, AccessType::kStore, 0}).complete_cycle;
  // Conflicting load evicts it: dw1 forwards the demand at depth 1 and sends a
  // writeback to dw2 for the dirty block, which also sits at depth 1.
  tl.Access(dw1, {0x4000, AccessType::kLoad, t});

  size_t writeback_matches = 0;
  uint32_t writeback_depth = 0;
  for (const auto& r : tl.trace.records) {
    if (!r.demand() && r.resource == "DW2") {
      ++writeback_matches;
      writeback_depth = r.depth;
    }
  }
  EXPECT_EQ(writeback_matches, 1u);
  EXPECT_EQ(writeback_depth, 1u);
}

TEST(Scratchpad, FixedCostNeverMissesAndIsContended) {
  OneAtATime tl;
  // A scratchpad: software-managed, fixed latency, no tags and no misses. It is
  // the sharpest test of the seam, sharing almost no implementation with a
  // cache — anything cache-shaped left in AccessNode would surface here.
  Scratchpad<"SPM"> spm(5);

  // Every access is a hit at the same cost, wherever it lands. A cache would
  // miss on the first touch of each line; a scratchpad never does.
  auto first = tl.Access(spm, {0x0000, AccessType::kLoad, 0});
  auto far = tl.Access(spm, {0xDEADBE00, AccessType::kLoad, 100});
  EXPECT_EQ(first.hit_level, "SPM");
  EXPECT_EQ(first.complete_cycle, 5u);
  EXPECT_EQ(far.hit_level, "SPM");
  EXPECT_EQ(far.complete_cycle, 105u);

  // It is a Target like any other: a second arrival while it is held waits.
  auto a = tl.Access(spm, {0x40, AccessType::kLoad, 1000});
  auto b = tl.Access(spm, {0x80, AccessType::kLoad, 1000});
  EXPECT_EQ(a.complete_cycle, 1005u);
  EXPECT_EQ(b.complete_cycle, 1010u);
}

TEST(AddressMap, RoutesByAddressCoversHalfOpenBounds) {
  OneAtATime tl;
  // An address map is itself a node: it routes by address and the caller says
  // nothing about which memory it meant. Two regions, two distinct backings,
  // distinguishable only by the latency each reports.
  MainMemory<"Fast"> fast(10);
  MainMemory<"Slow"> slow(200);
  AddressMap router;
  router.Map(0x0000, 0x1000, &fast);  // [0x0000, 0x1000)
  router.Map(0x1000, 0x2000, &slow);  // [0x1000, 0x2000)

  auto lo = tl.Access(router, {0x0500, AccessType::kLoad, 0});
  auto hi = tl.Access(router, {0x1500, AccessType::kLoad, 0});
  EXPECT_EQ(lo.hit_level, "Fast");
  EXPECT_EQ(lo.complete_cycle, 10u);
  EXPECT_EQ(hi.hit_level, "Slow");
  EXPECT_EQ(hi.complete_cycle, 200u);

  // A host that must reject bad configuration where asserts are compiled out
  // can ask first. The answer respects the same half-open bounds.
  EXPECT_TRUE(router.Covers(0x0000));
  EXPECT_TRUE(router.Covers(0x1FFF));
  EXPECT_FALSE(router.Covers(0x2000));
  EXPECT_FALSE(router.Covers(0x9999));

  // Boundaries are half-open: the end of one region is the start of the next,
  // so adjacent regions neither overlap nor leave a gap.
  auto first = tl.Access(router, {0x0000, AccessType::kLoad, 100});
  auto boundary = tl.Access(router, {0x1000, AccessType::kLoad, 100});
  EXPECT_EQ(first.hit_level, "Fast");
  EXPECT_EQ(boundary.hit_level, "Slow");
}

TEST(AddressMap, PreservesAddressDownstream) {
  OneAtATime tl;
  // Routing is transparent to everything downstream: a cache behind the map
  // still sees the original address, so its own set/tag split is unchanged.
  MainMemory<"Behind"> behind(100);
  Cache<"Cached", 1, 2, 64, LRUPolicy, 1> cached(&behind);
  AddressMap mapped;
  mapped.Map(0x0000, 0x10000, &cached);
  auto cold = tl.Access(mapped, {0x2000, AccessType::kLoad, 0});
  auto warm = tl.Access(mapped, {0x2000, AccessType::kLoad, 500});
  EXPECT_EQ(cold.hit_level, "Behind");
  EXPECT_EQ(cold.complete_cycle, 101u);
  EXPECT_EQ(warm.hit_level, "Cached");
  EXPECT_EQ(warm.complete_cycle, 501u);
}

TEST(AddressMap, ScratchpadSitsBehindAnyRegionBase) {
  OneAtATime tl;
  // A scratchpad behind a region that does not start at zero. AddressMap passes
  // addresses through untouched, so the node indexes nothing, so an absolute
  // address is simply what arrives.
  Scratchpad<"High"> high(5);
  AddressMap high_map;
  high_map.Map(0x80000000, 0x80001000, &high);
  auto r = tl.Access(high_map, {0x80000800, AccessType::kLoad, 0});
  EXPECT_EQ(r.hit_level, "High");
  EXPECT_EQ(r.complete_cycle, 5u);
}

TEST(AddressMap, MixedCacheAndScratchpadTopology) {
  OneAtATime tl;
  // A topology mixing both: a cache backed by a scratchpad rather than a
  // memory, and both reachable through one address map. If the seam were
  // cache-shaped, a scratchpad could not stand in for a successor.
  Scratchpad<"Backing"> backing(8);
  Cache<"Over", 1, 2, 64, LRUPolicy, 2> over(&backing);
  Scratchpad<"Direct"> direct(3);
  AddressMap map;
  map.Map(0x0000, 0x1000, &over);
  map.Map(0x1000, 0x2000, &direct);

  auto miss = tl.Access(map, {0x0000, AccessType::kLoad, 0});   // 2 + 8
  auto hit = tl.Access(map, {0x0000, AccessType::kLoad, 500});  // 2
  auto spm = tl.Access(map, {0x1800, AccessType::kLoad, 900});  // 3
  EXPECT_EQ(miss.hit_level, "Backing");
  EXPECT_EQ(miss.complete_cycle, 10u);
  EXPECT_EQ(hit.hit_level, "Over");
  EXPECT_EQ(hit.complete_cycle, 502u);
  EXPECT_EQ(spm.hit_level, "Direct");
  EXPECT_EQ(spm.complete_cycle, 903u);
}

TEST(Cache, DirtyEvictionHoldsTheLevelBelow) {
  OneAtATime tl;
  // A dirty eviction costs the level below real time. The requester does not
  // wait for it — the writeback leaves after the fill has arrived — but the
  // level below is held, so the next access there queues behind it.
  MainMemory<"WMem"> wmem(100);
  Cache<"W1", 1, 1, 64, LRUPolicy, 1> w1(&wmem);

  // Dirty the only line: miss through to memory, 1 + 100.
  Cycle t = tl.Access(w1, {0x0000, AccessType::kStore, 0}).complete_cycle;
  ASSERT_EQ(t, 101u);

  // Conflicting load evicts the dirty line. The requester's own completion is
  // the demand path only: 101 + 1 + 100 = 202. It must not include the
  // writeback, which leaves afterwards.
  auto evicting = tl.Access(w1, {0x4000, AccessType::kLoad, t});
  EXPECT_EQ(evicting.complete_cycle, 202u);

  // But memory IS held by that writeback, from 202 to 302. A load issued at 202
  // cannot start there until 302, so it completes at 302 + 100 = 402 rather
  // than the 303 an unoccupied memory would give.
  auto queued = tl.Access(w1, {0x8000, AccessType::kLoad, 202});
  EXPECT_EQ(queued.complete_cycle, 402u);
}

TEST(Cache, CleanEvictionHoldsNothing) {
  OneAtATime tl;
  // A clean eviction costs nothing: there is nothing to write back, so the
  // level below is not held and the next access does not wait.
  MainMemory<"CMem"> cmem(100);
  Cache<"C1", 1, 1, 64, LRUPolicy, 1> c1(&cmem);
  Cycle t = tl.Access(c1, {0x0000, AccessType::kLoad, 0}).complete_cycle;
  t = tl.Access(c1, {0x4000, AccessType::kLoad, t})
          .complete_cycle;  // clean evict
  auto after = tl.Access(c1, {0x8000, AccessType::kLoad, t});
  EXPECT_EQ(after.complete_cycle, t + 101);
}

TEST(Cache, WritebackIsReturnedFromCompleteWithNoEngineInvolved) {
  // Complete returns the traffic a fill causes rather than sending it through
  // an engine-provided callback, so this — a cache's writeback behaviour — is
  // testable entirely on its own: construct the node, drive it directly,
  // inspect the return. No EventEngine exists anywhere in this test.
  MainMemory<"DirectMem"> mem(100);
  Cache<"Direct", 1, 1, 64, LRUPolicy, 1> cache(&mem);

  // Fill the only line: nothing resident yet, so nothing is evicted and
  // Complete has no writeback to report.
  const Request fill{.addr = 0x0000, .type = AccessType::kStore};
  const Route miss = cache.Serve(fill);
  ASSERT_NE(miss.next, nullptr);
  EXPECT_TRUE(cache.Complete(fill, /*forwarded=*/true).empty());

  // A conflicting store evicts that now-dirty line: Complete must report
  // exactly one writeback, to the successor this cache was constructed with, at
  // the victim's own address.
  const Request evict{.addr = 0x4000, .type = AccessType::kStore};
  const Route second_miss = cache.Serve(evict);
  ASSERT_NE(second_miss.next, nullptr);
  const std::vector<Writeback> writebacks =
      cache.Complete(evict, /*forwarded=*/true);
  ASSERT_EQ(writebacks.size(), 1u);
  EXPECT_EQ(writebacks[0].to, &mem);
  EXPECT_EQ(writebacks[0].request.addr, 0x0000u);
  EXPECT_EQ(writebacks[0].request.type, AccessType::kStore);
}

TEST(Cache, FifoEvictsByInsertionOrderNotRecency) {
  OneAtATime tl;
  // FIFO ignores hits: unlike LRU, touching a line does not save it from
  // eviction — only insertion order matters. Same access pattern as the LRU
  // eviction test above, but the outcome is the opposite: way 0 (0x0000, filled
  // first) is evicted despite being re-touched, not way 1.
  MainMemory<"FMem"> fmem(100);
  Cache<"Fifo", 1, 2, 64, FIFOPolicy, 1> fifo(&fmem);

  tl.Access(fifo, {0x0000, AccessType::kLoad, 0});  // fills way 0
  tl.Access(fifo, {0x0040, AccessType::kLoad, 0});  // fills way 1, set is full
  tl.Access(fifo, {0x0000, AccessType::kLoad, 0});  // hit; FIFO does not care

  // New block: FIFO evicts way 0 (oldest by insertion) regardless of the hit
  // above.
  tl.Access(fifo, {0x0080, AccessType::kLoad, 0});

  // Check survival first: it is a hit and does not itself evict anything.
  // Checking eviction first would miss and evict the survivor before this read
  // ran, since the set is still full.
  auto survived = tl.Access(fifo, {0x0040, AccessType::kLoad, 0});
  auto evicted = tl.Access(fifo, {0x0000, AccessType::kLoad, 0});
  EXPECT_EQ(survived.hit_level, "Fifo");
  EXPECT_EQ(evicted.hit_level, "FMem");
}

TEST(Policies, RandomPolicyStaysInRangeAndSelectsMoreThanOneVictim) {
  // Every victim selection must land inside the valid way range, and enough
  // fills must produce more than one distinct victim way — otherwise it is not
  // actually random, just a constant in disguise.
  RandomPolicy::State</*Sets=*/1, /*Ways=*/8> rp;
  std::vector<bool> seen(8, false);
  for (int i = 0; i < 200; ++i) {
    size_t victim = rp.GetVictim(0);
    ASSERT_LT(victim, 8u);
    seen[victim] = true;
  }
  size_t distinct = 0;
  for (bool v : seen) {
    if (v) ++distinct;
  }
  EXPECT_GT(distinct, 1u);
}

TEST(Cache, RandomPolicyWiresThroughCacheWithoutError) {
  OneAtATime tl;
  // Wired through Cache like any other policy: fills and evictions on a real
  // cache must not crash and must still return sensible hit levels.
  MainMemory<"RMem2"> rmem2(100);
  Cache<"Rand", 1, 4, 64, RandomPolicy, 1> rc(&rmem2);
  for (uint64_t block = 0; block < 8; ++block) {
    auto res = tl.Access(rc, {block * 64, AccessType::kLoad, 0});
    EXPECT_TRUE(res.hit_level == "Rand" || res.hit_level == "RMem2")
        << "got " << res.hit_level;
  }
}

// Runs one fixed trace through a fresh random-replacement cache and returns the
// whole event stream it recorded, one line per event. Twelve distinct blocks
// cycled through a four-way set evict on nearly every access, so the victim
// choice decides almost every outcome; every third access is a store, so
// evictions are often dirty and writebacks join the stream too.
template <typename Policy>
std::vector<std::string> RandomReplacementRecords() {
  MainMemory<"RMem"> mem(100);
  Cache<"Rand", 1, 4, 64, Policy, 1> cache(&mem);
  EventEngine engine;
  Initiator agent(engine, 0);
  for (uint64_t i = 0; i < 200; ++i) {
    const uint64_t block = (i * 7) % 12;
    agent.Issue(cache, block * 64,
                i % 3 == 0 ? AccessType::kStore : AccessType::kLoad);
  }
  RecordingSink trace;
  RunChecked(engine, &trace);

  std::vector<std::string> events;
  for (const Record& r : trace.records) {
    events.push_back(
        std::string(r.resource) + " " + std::to_string(r.addr) +
        (r.forwarded ? " miss " : " hit ") + std::to_string(r.arrival.value()) +
        ".." + std::to_string(r.finish.value()) +
        (r.demand() ? "" : " writeback") + " depth " + std::to_string(r.depth));
  }
  return events;
}

TEST(Cache, RandomReplacementIsReproducibleRunToRun) {
  // A simulator whose result changes between two runs of the same trace cannot
  // be compared against anything, including itself.
  EXPECT_EQ(RandomReplacementRecords<RandomPolicy>(),
            RandomReplacementRecords<RandomPolicy>());
}

TEST(Cache, RandomReplacementSeedIsPartOfTheStructure) {
  // The seed is a choice made when the simulator is built, like the policy
  // itself. Choosing another one explores another sequence of victims.
  EXPECT_EQ(RandomReplacementRecords<SeededRandomPolicy<7>>(),
            RandomReplacementRecords<SeededRandomPolicy<7>>());
  EXPECT_NE(RandomReplacementRecords<SeededRandomPolicy<7>>(),
            RandomReplacementRecords<SeededRandomPolicy<8>>());
}

TEST(Timeline, CompletionNearTheEndOfTimeSaturatesRatherThanWraps) {
  // A completion cycle that wrapped past the top of the range would read as an
  // access finishing almost at once — a fast, plausible, wrong answer. Time
  // saturates instead, at every kind of node that adds its own cost.
  const uint64_t late = UINT64_MAX - 10;
  MainMemory<"LateMem"> mem(100);
  Scratchpad<"LateSpm"> spm(100);
  MainMemory<"BelowCache"> below(1);
  Cache<"LateCache", 1, 1, 64, LRUPolicy, 100> cache(&below);

  for (AccessNode* node :
       {static_cast<AccessNode*>(&mem), static_cast<AccessNode*>(&spm),
        static_cast<AccessNode*>(&cache)}) {
    EventEngine engine;
    Initiator agent(engine, 0, late);
    const EventId first = agent.Issue(*node, 0x40, AccessType::kLoad);
    // Every access after it waits on it rather than jumping back in time.
    const EventId second = agent.Issue(*node, 0x40, AccessType::kLoad);
    RunChecked(engine);
    EXPECT_EQ(engine.CompletionOf(first).value(), UINT64_MAX)
        << node->NodeName();
    EXPECT_EQ(engine.CompletionOf(second).value(), UINT64_MAX)
        << node->NodeName();
  }
}

#ifdef NDEBUG
TEST(AddressMap, UnmappedAccessInReleaseNeverMakesTheNextOneLookFast) {
  // With asserts compiled out an unmapped access still returns: its result
  // saturates, and whatever the initiator issues next must not wrap around to a
  // small completion cycle because of it.
  MainMemory<"Mapped"> mem(100);
  AddressMap router;
  router.Map(0x0000, 0x1000, &mem);
  EventEngine engine;
  Initiator agent(engine, 0);

  const EventId unmapped = agent.Issue(router, 0x9000, AccessType::kLoad);
  const EventId next = agent.Issue(router, 0x0040, AccessType::kLoad);
  RunChecked(engine);
  EXPECT_EQ(engine.CompletionOf(unmapped).value(), UINT64_MAX);
  EXPECT_EQ(engine.CompletionOf(next).value(), UINT64_MAX);
}

TEST(AddressMap, UnmappedAccessInReleaseHoldsNothingOthersNeed) {
  // The bad access saturates, but only its own requester pays: another
  // initiator routed through the same map still reaches mapped memory on time,
  // because the map itself is not held by the access it could not route.
  MainMemory<"Mapped"> mem(100);
  AddressMap router;
  router.Map(0x0000, 0x1000, &mem);
  EventEngine engine;
  Initiator bad(engine, 0);
  Initiator good(engine, 1, /*ready_at=*/10);

  const EventId unmapped = bad.Issue(router, 0x9000, AccessType::kLoad);
  const EventId mapped = good.Issue(router, 0x0040, AccessType::kLoad);
  const RunResult run = RunChecked(engine);
  EXPECT_EQ(engine.CompletionOf(unmapped).value(), UINT64_MAX);
  EXPECT_EQ(engine.CompletionOf(mapped).value(), 110u);
  for (const auto& result : run.completed) {
    if (result.event_id == unmapped) EXPECT_EQ(result.served_by, "<unmapped>");
  }
}
#endif

TEST(Initiator, CannotIssueBeforeItIsFree) {
  // An Initiator is in order with one access outstanding: each access it issues
  // begins only once the previous completed. With one initiator alone, its
  // clock is simply where its last access finished.
  MainMemory<"IMem"> imem(100);
  Cache<"I1", 1, 2, 64, LRUPolicy, 1> i1(&imem);
  EventEngine engine;
  Initiator solo(engine, 0);
  const uint64_t addrs[] = {0x0000, 0x0040, 0x0000};
  EventId ids[3];
  for (size_t k = 0; k < 3; ++k) {
    ids[k] = solo.Issue(i1, addrs[k], AccessType::kLoad);
  }
  RunChecked(engine);

  const uint64_t want[] = {101, 202, 203};
  for (size_t k = 0; k < 3; ++k) {
    EXPECT_EQ(engine.CompletionOf(ids[k]).value(), want[k])
        << "access index " << k;
  }
  EXPECT_EQ(solo.BusyUntil(), 203u);
}

TEST(Initiator, KeepsIssuingInOrderAcrossWindows) {
  // An initiator's previous access may be retired by the time it issues the
  // next: the engine has run past it and forgotten its cycle. The next access
  // still begins no earlier than the window, as the host promised.
  MainMemory<"WMem"> wmem(100);
  EventEngine engine;
  Initiator core(engine, 0);
  const EventId first = core.Issue(wmem, 0x0000, AccessType::kLoad);
  RunChecked(engine, 150);  // first completed at 100 and is retired
  EXPECT_EQ(engine.CompletionOf(first).error(), Absent::kRetired);
  EXPECT_EQ(core.BusyUntil(), 150u);

  core.Issue(wmem, 0x0040, AccessType::kLoad);
  RunChecked(engine);
  EXPECT_EQ(core.BusyUntil(), 250u);
}

TEST(Initiator, TwoInitiatorsContendForOneMemory) {
  // Two initiators sharing one memory advance against one timeline. Both arrive
  // at 0; the memory serves one 0..100 and the other 100..200, and each
  // initiator's own clock advances only by its own access.
  MainMemory<"SMem"> smem(100);
  EventEngine engine;
  Initiator core(engine, 0);
  Initiator dma(engine, 1);
  core.Issue(smem, 0x0000, AccessType::kLoad);
  dma.Issue(smem, 0x1000, AccessType::kLoad);
  RunChecked(engine);
  EXPECT_EQ(core.BusyUntil(), 100u);
  EXPECT_EQ(dma.BusyUntil(), 200u);
}

TEST(Initiator, PriorityBreaksTiesAtANode) {
  // Two accesses arriving at one node at the same cycle go to the
  // higher-priority initiator first — DMA yields to cores — rather than falling
  // out of submission order.
  MainMemory<"PMem"> pmem(100);
  EventEngine engine;
  Initiator low_prio_dma(engine, 0, /*ready_at=*/0, /*priority=*/0);
  Initiator high_prio_core(engine, 1, /*ready_at=*/0, /*priority=*/1);

  // Submitted DMA-first, so an order-based tie-break would wrongly serve it.
  low_prio_dma.Issue(pmem, 0x0000, AccessType::kLoad);
  high_prio_core.Issue(pmem, 0x1000, AccessType::kLoad);
  RunChecked(engine);
  EXPECT_EQ(high_prio_core.BusyUntil(), 100u);
  EXPECT_EQ(low_prio_dma.BusyUntil(), 200u);
}

TEST(Initiator, PriorityNeverOverridesAnEarlierArrival) {
  // Not a tie: an access that arrived first is served first, whatever the
  // priority of one arriving while it waits or is served.
  MainMemory<"OMem"> omem(100);
  EventEngine engine;
  Initiator earlier_low_prio(engine, 0, /*ready_at=*/0, /*priority=*/0);
  Initiator later_high_prio(engine, 1, /*ready_at=*/50, /*priority=*/1);
  later_high_prio.Issue(omem, 0x1000, AccessType::kLoad);
  earlier_low_prio.Issue(omem, 0x0000, AccessType::kLoad);
  RunChecked(engine);
  EXPECT_EQ(earlier_low_prio.BusyUntil(), 100u);
  EXPECT_EQ(later_high_prio.BusyUntil(), 200u);
}

TEST(Initiator, SeveralInitiatorsShareOneMemoryInArrivalOrder) {
  // Several agents on one timeline, a runtime-sized set of them. Each issues
  // twice; the memory serves whoever arrived first, so each agent's second
  // access queues behind the others' first.
  MainMemory<"RMem"> rmem(100);
  EventEngine engine;
  std::vector<Initiator> agents;
  for (uint32_t id = 0; id < 3; ++id) agents.emplace_back(engine, id);
  for (int round = 0; round < 2; ++round) {
    for (auto& a : agents) {
      a.Issue(rmem, 0x40 * a.Id().value(), AccessType::kLoad);
    }
  }
  const RunResult run = RunChecked(engine);

  // Round-robin falls out of arrival order: all start at 0, each access costs
  // 100 and the memory serialises them, so the three take turns.
  std::vector<uint32_t> order;
  for (const auto& result : run.completed) {
    order.push_back(result.initiator.value());
  }
  const std::vector<uint32_t> want = {0, 1, 2, 0, 1, 2};
  EXPECT_EQ(order, want);
  EXPECT_EQ(agents[0].BusyUntil(), 400u);
  EXPECT_EQ(agents[1].BusyUntil(), 500u);
  EXPECT_EQ(agents[2].BusyUntil(), 600u);
}

TEST(Initiator, EarlierArrivalIsNotQueuedBehindALaterOne) {
  // Two initiators share one memory. A reaches it through a slow cache, B
  // directly. A is submitted first, but B arrives at the memory before A does,
  // so it must not queue behind A.
  //
  //   A: ready 0 -> lookup 0..50 -> arrives at memory at 50
  //   B: ready 10                 -> arrives at memory at 10
  //
  // Served in arrival order: B at 10..110, A at 110..210. Timed in call order,
  // as the old node model did, B completed at 250 instead.
  MainMemory<"Shared"> shared(100);
  Cache<"SlowL1", 1, 1, 64, LRUPolicy, 50> slow_l1(&shared);
  EventEngine engine;
  Initiator a(engine, 0);
  Initiator b(engine, 1, /*ready_at=*/10);
  a.Issue(slow_l1, 0x0000, AccessType::kLoad);
  b.Issue(shared, 0x1000, AccessType::kLoad);
  RunChecked(engine);

  EXPECT_EQ(b.BusyUntil(), 110u);
  EXPECT_EQ(a.BusyUntil(), 210u);
}

TEST(Cache, ConcurrentMissesToOneLineFillItOnce) {
  // Two requesters miss on the same line at once. Both fetch it, but the second
  // fill finds it already resident and keeps that copy rather than placing a
  // duplicate in the set's other way. So a later line fills the free way,
  // evicting nothing — no writeback of the dirty line.
  MainMemory<"CMem"> cmem(100);
  Cache<"Shared2", 1, 2, 64, LRUPolicy, 1> shared(&cmem);
  EventEngine engine;
  RecordingSink trace;
  // A checker kept across both runs below: a's second access depends on its
  // first, which only the same checker instance can still know completed — see
  // checked_run.hpp's note on multi-call tests.
  CheckingSink checker;
  Initiator a(engine, 0);
  Initiator b(engine, 1);
  a.Issue(shared, 0x0000, AccessType::kStore);
  b.Issue(shared, 0x0000, AccessType::kStore);
  {
    BroadcastSink both(&trace, &checker);
    engine.RunUntilIdle(&both);
  }
  a.Issue(shared, 0x0040, AccessType::kLoad);
  {
    BroadcastSink both(&trace, &checker);
    engine.RunUntilIdle(&both);
  }
  for (const auto& v : checker.Violations()) {
    ADD_FAILURE() << v.what;
  }

  size_t writebacks = 0;
  for (const auto& r : trace.records) {
    if (!r.demand()) ++writebacks;
  }
  EXPECT_EQ(writebacks, 0u);
}

TEST(Initiator, BackgroundTransferOverlappingComputeIsVisible) {
  // A background transfer overlapping compute, and whether it was hidden. The
  // DMA holds the memory while the core works out of its own scratchpad, so the
  // two overlap in the record rather than serialising.
  MainMemory<"Far"> far(200);
  Scratchpad<"Local"> local(4);
  EventEngine engine;
  Initiator dma(engine, 0);
  Initiator core(engine, 1);

  // The DMA starts a long transfer at 0; the core computes locally meanwhile.
  dma.Issue(far, 0x0000, AccessType::kLoad);
  for (uint64_t k = 0; k < 3; ++k) {
    core.Issue(local, 0x40 * k, AccessType::kLoad);
  }
  RecordingSink trace;
  RunChecked(engine, &trace);
  const Cycle transfer = dma.BusyUntil();
  const Cycle compute = core.BusyUntil();

  // The transfer ends at 200; the core's three local accesses end at 12. The
  // compute finished first, so the transfer was NOT hidden by it.
  EXPECT_EQ(transfer, 200u);
  EXPECT_EQ(compute, 12u);
  EXPECT_LT(compute, transfer);

  size_t far_events = 0, local_events = 0;
  for (const auto& r : trace.records) {
    if (r.resource == "Far") ++far_events;
    if (r.resource == "Local") ++local_events;
  }
  EXPECT_EQ(far_events, 1u);
  EXPECT_EQ(local_events, 3u);
}

#ifdef TIMEBALL_WITH_SQLITE

// A database path in the test's scratch directory, emptied first.
static std::string FreshDb(const char* name) {
  const std::string path = ::testing::TempDir() + name;
  std::remove(path.c_str());
  return path;
}

TEST(EventStore, LeavesOneRowPerResourceOccupiedAndATaskTable) {
  // A run leaves behind one SQLite file: a row each time a resource was
  // occupied, and tasks as cycle intervals rather than a name repeated on every
  // row.
  const std::string db_path = FreshDb("timeball_ops.sqlite");
  {
    EventStore store(db_path);
    MainMemory<"QMem"> qmem(100);
    Cache<"Q1", 1, 2, 64, LRUPolicy, 4> q1(&qmem);
    EventEngine engine;
    Initiator agent(engine, 0);
    // Kept across both runs below: the "steady" batch's first access depends on
    // the "warmup" batch's, which only the same checker instance still knows
    // completed.
    CheckingSink checker;

    store.BeginTask("warmup", 0);
    agent.Issue(q1, 0x0000, AccessType::kLoad);  // miss: 0..104
    RunChecked(engine, checker, &store);
    store.EndTask(agent.BusyUntil());
    store.BeginTask("steady", agent.BusyUntil());
    agent.Issue(q1, 0x0000, AccessType::kLoad);  // hit: 104..108
    agent.Issue(q1, 0x0040, AccessType::kLoad);  // miss
    RunChecked(engine, checker, &store);
    store.EndTask(agent.BusyUntil());
    ReportViolations(checker);
  }  // store flushes and closes here

  // The first access occupied both levels, the hit only Q1, the third both.
  EXPECT_EQ(SqliteScalar(db_path, "SELECT COUNT(*) FROM ops"), 5);
  EXPECT_EQ(SqliteScalar(db_path, "SELECT COUNT(*) FROM tasks"), 2);

  // The join is the point: time attributed to a named task, without the name
  // appearing on any row.
  const int steady_rows =
      SqliteScalar(db_path,
                   "SELECT COUNT(*) FROM ops o JOIN tasks t "
                   "ON o.start >= t.begin_cycle AND o.start < t.end_cycle "
                   "WHERE t.name = 'steady'");
  EXPECT_EQ(steady_rows, 3);
}

TEST(EventStore, AnswersWhichOperationWasSlowAndWhy) {
  // Static operations and memory accesses land in one database, with what each
  // waited on, so a query can find the slowest operation, split its time into
  // waiting and working, and name what held it up.
  const std::string db_path = FreshDb("timeball_slow.sqlite");
  {
    EventStore store(db_path);
    MainMemory<"DRAM"> dram(100);
    Cache<"L1", 1, 2, 64, LRUPolicy, 4> l1(&dram);
    EventEngine engine;
    const ResourceId dma = engine.AddResource({"dma", 1});
    const ResourceId mac = engine.AddResource({"mac", 1});

    // Two transfers share one DMA, so the second waits; the multiply depends on
    // both and on a load through the cache.
    const EventId a = engine.Submit({"load_A", dma, 0, 148, {0, {}}});
    const EventId b = engine.Submit({"load_B", dma, 0, 148, {0, {}}});
    const EventId w = engine.SubmitAccess(l1, {.addr = 0x0000});
    engine.Submit({"matmul", mac, 0, 256, {0, {a, b, w}}});
    RunChecked(engine, &store);
  }

  // The slowest operation end to end — an access spans several rows, so group
  // by operation — is the second transfer, not the multiply that did the most
  // work: it queued for the DMA.
  EXPECT_EQ(SqliteScalar(db_path,
                         "SELECT op FROM ops WHERE op != 0 GROUP BY op "
                         "ORDER BY MAX(finish) - MIN(arrival) DESC LIMIT 1"),
            2);  // load_B: 0..296

  // The second transfer queued for the DMA behind the first, so half its time
  // was waiting. The multiply arrives only once its last dependency has
  // finished, so all of its time is work.
  EXPECT_EQ(SqliteScalar(db_path,
                         "SELECT start - arrival FROM ops "
                         "WHERE name = 'load_B'"),
            148);  // waiting
  EXPECT_EQ(SqliteScalar(db_path,
                         "SELECT finish - start FROM ops "
                         "WHERE name = 'load_B'"),
            148);  // working
  EXPECT_EQ(SqliteScalar(db_path,
                         "SELECT finish - start FROM ops "
                         "WHERE name = 'matmul'"),
            256);

  // The multiply waited on three things, and the last to finish is what held it
  // up: the queued transfer.
  EXPECT_EQ(SqliteScalar(db_path,
                         "SELECT COUNT(*) FROM deps d JOIN ops o "
                         "ON d.op = o.op WHERE o.name = 'matmul'"),
            3);
  EXPECT_EQ(SqliteScalar(db_path,
                         "SELECT p.op FROM deps d "
                         "JOIN ops o ON d.op = o.op "
                         "JOIN ops p ON p.op = d.depends_on "
                         "WHERE o.name = 'matmul' "
                         "ORDER BY p.finish DESC LIMIT 1"),
            2);  // load_B

  // An access is one row per hop; its span is first arrival to last finish.
  EXPECT_EQ(SqliteScalar(db_path,
                         "SELECT MAX(finish) - MIN(arrival) FROM ops "
                         "WHERE op = 3"),
            104);
}

TEST(EventStore, UnclosedTaskSpansToTheLastCycleRecorded) {
  // A task the host never closes spans to the last cycle recorded, not to its
  // own begin. A zero-length interval joins against nothing, so every row in
  // that span would be unattributable.
  const std::string db_path = FreshDb("timeball_unclosed.sqlite");
  {
    EventStore store(db_path);
    MainMemory<"UMem"> umem(100);
    EventEngine engine;
    Initiator agent(engine, 0);
    store.BeginTask("never_closed", 0);
    agent.Issue(umem, 0x0000, AccessType::kLoad);  // 0..100
    RunChecked(engine, &store);
  }  // destructor closes the task
  const int spanned =
      SqliteScalar(db_path,
                   "SELECT COUNT(*) FROM ops o JOIN tasks t "
                   "ON o.start >= t.begin_cycle AND o.start < t.end_cycle");
  EXPECT_EQ(spanned, 1);
}

TEST(EventStore, UnopenableStoreReportsItAndNoOps) {
  // A path that cannot be opened is a runtime condition, not a topology
  // mistake: IsOpen reports it and every later call is a no-op, rather than
  // asserting away under NDEBUG and leaving a run with nothing behind.
  EventStore bad("/nonexistent_dir_xyz/events.sqlite");
  MainMemory<"BMem"> bmem(100);
  EventEngine engine;
  Initiator agent(engine, 0);
  bad.BeginTask("doomed", 0);
  agent.Issue(bmem, 0x0000, AccessType::kLoad);
  RunChecked(engine, &bad);
  bad.EndTask(100);
  EXPECT_FALSE(bad.IsOpen());
}

TEST(EventStore, ResourceRowsDoNotDuplicateOperationDependencies) {
  const std::string db_path = FreshDb("timeball_resource_sets.sqlite");
  EventStore store(db_path);
  ASSERT_TRUE(store.IsOpen());
  EventEngine engine;
  const ResourceId a = engine.AddResource({"a", 1});
  const ResourceId b = engine.AddResource({"b", 1});
  const EventId first = engine.Submit({"first", a, 0, 3, {0, {}}});
  engine.Submit({"joined", a, 0, 2, {0, {first}}, {b}});
  RunChecked(engine, &store);
  ASSERT_TRUE(store.Close()) << store.Error();

  EXPECT_EQ(
      SqliteScalar(db_path, "SELECT COUNT(*) FROM ops WHERE name = 'joined'"),
      2);
  EXPECT_EQ(SqliteScalar(db_path, "SELECT COUNT(*) FROM deps"), 1);
}

TEST(EventStore, BorrowedDatabaseLeavesTheTransactionWithItsOwner) {
  sqlite3* raw = nullptr;
  ASSERT_EQ(sqlite3_open(":memory:", &raw), SQLITE_OK);
  const std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw,
                                                              sqlite3_close);
  ASSERT_EQ(sqlite3_exec(db.get(), "BEGIN", nullptr, nullptr, nullptr),
            SQLITE_OK);
  {
    EventStore store(*db);
    ASSERT_TRUE(store.IsOpen()) << store.Error();
    EventEngine engine;
    const ResourceId resource = engine.AddResource({"resource", 1});
    engine.Submit({"work", resource, 0, 3, {0, {}}});
    RunChecked(engine, &store);
    ASSERT_TRUE(store.Close()) << store.Error();
  }
  EXPECT_EQ(sqlite3_get_autocommit(db.get()), 0);
  EXPECT_EQ(sqlite3_exec(db.get(),
                         "CREATE TABLE labels(op INTEGER, label TEXT);"
                         "INSERT INTO labels VALUES(1, 'sample');"
                         "COMMIT",
                         nullptr, nullptr, nullptr),
            SQLITE_OK);
  sqlite3_stmt* statement = nullptr;
  ASSERT_EQ(sqlite3_prepare_v2(db.get(),
                               "SELECT COUNT(*) FROM ops JOIN labels USING(op)",
                               -1, &statement, nullptr),
            SQLITE_OK);
  const std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> query(
      statement, sqlite3_finalize);
  ASSERT_EQ(sqlite3_step(query.get()), SQLITE_ROW);
  EXPECT_EQ(sqlite3_column_int(query.get(), 0), 1);
}

TEST(EventStore, BorrowedInitializationFailureLeavesTheOwnerInControl) {
  sqlite3* raw = nullptr;
  ASSERT_EQ(sqlite3_open(":memory:", &raw), SQLITE_OK);
  const std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw,
                                                              sqlite3_close);
  ASSERT_EQ(sqlite3_exec(db.get(), "BEGIN", nullptr, nullptr, nullptr),
            SQLITE_OK);
  const int limit = sqlite3_limit(db.get(), SQLITE_LIMIT_SQL_LENGTH, 1);
  {
    EventStore store(*db);
    EXPECT_FALSE(store.IsOpen());
    EXPECT_FALSE(store.Error().empty());
    EXPECT_FALSE(store.Close());
  }
  EXPECT_EQ(sqlite3_get_autocommit(db.get()), 0);
  sqlite3_limit(db.get(), SQLITE_LIMIT_SQL_LENGTH, limit);
  EXPECT_EQ(sqlite3_exec(db.get(), "ROLLBACK", nullptr, nullptr, nullptr),
            SQLITE_OK);
}

TEST(EventStore, InvalidDatabaseReportsInitializationFailure) {
  const std::string db_path = FreshDb("timeball_invalid.sqlite");
  std::FILE* file = std::fopen(db_path.c_str(), "wb");
  ASSERT_NE(file, nullptr);
  ASSERT_GE(std::fputs("not a SQLite database", file), 0);
  ASSERT_EQ(std::fclose(file), 0);

  EventStore store(db_path);
  EXPECT_FALSE(store.IsOpen());
  EXPECT_FALSE(store.Error().empty());
}

TEST(EventStore, WriteFailureIsReportedAfterOpening) {
  const std::string db_path = FreshDb("timeball_locked.sqlite");
  EventStore store(db_path);
  ASSERT_TRUE(store.IsOpen());

  sqlite3* blocker = nullptr;
  ASSERT_EQ(sqlite3_open(db_path.c_str(), &blocker), SQLITE_OK);
  ASSERT_EQ(sqlite3_exec(blocker, "BEGIN EXCLUSIVE", nullptr, nullptr, nullptr),
            SQLITE_OK);

  EventEngine engine;
  const ResourceId resource = engine.AddResource({"core", 1});
  engine.Submit({"work", resource, 0, 1, {0, {}}});
  engine.RunUntilIdle(&store);

  EXPECT_FALSE(store.IsOpen());
  EXPECT_FALSE(store.Error().empty());
  EXPECT_EQ(sqlite3_exec(blocker, "ROLLBACK", nullptr, nullptr, nullptr),
            SQLITE_OK);
  EXPECT_EQ(sqlite3_close(blocker), SQLITE_OK);
  EXPECT_FALSE(store.Close());
  EXPECT_FALSE(store.Close());
}

TEST(EventStore, CloseReportsCommittedRecords) {
  const std::string db_path = FreshDb("timeball_closed.sqlite");
  EventStore store(db_path);
  ASSERT_TRUE(store.IsOpen());

  EventEngine engine;
  const ResourceId resource = engine.AddResource({"core", 1});
  engine.Submit({"work", resource, 0, 1, {0, {}}});
  engine.RunUntilIdle(&store);

  EXPECT_TRUE(store.Close());
  EXPECT_TRUE(store.Close());
  EXPECT_TRUE(store.Error().empty());
  EXPECT_EQ(SqliteScalar(db_path, "SELECT COUNT(*) FROM ops"), 1);
}

TEST(EventStore, CloseReportsCommitFailure) {
  const std::string db_path = FreshDb("timeball_commit_locked.sqlite");
  EventStore store(db_path);
  ASSERT_TRUE(store.IsOpen());

  EventEngine engine;
  const ResourceId resource = engine.AddResource({"core", 1});
  engine.Submit({"work", resource, 0, 1, {0, {}}});
  engine.RunUntilIdle(&store);

  sqlite3* blocker = nullptr;
  ASSERT_EQ(sqlite3_open(db_path.c_str(), &blocker), SQLITE_OK);
  ASSERT_EQ(sqlite3_exec(blocker, "BEGIN; SELECT COUNT(*) FROM ops", nullptr,
                         nullptr, nullptr),
            SQLITE_OK);

  EXPECT_FALSE(store.Close());
  EXPECT_FALSE(store.Error().empty());
  EXPECT_EQ(sqlite3_exec(blocker, "ROLLBACK", nullptr, nullptr, nullptr),
            SQLITE_OK);
  EXPECT_EQ(sqlite3_close(blocker), SQLITE_OK);
}

#endif  // TIMEBALL_WITH_SQLITE
