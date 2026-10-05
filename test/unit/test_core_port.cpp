// Copyright 2025-2026 Yi-Ping Pan (Cloudlet)

#include <cstdint>
#include <vector>

#include "checked_run.hpp"
#include "gtest/gtest.h"
#include "timeball/address_map.hpp"
#include "timeball/core_port.hpp"
#include "timeball/timeball.hpp"

using namespace timeball;
using namespace timeball::test;

namespace {

// A MAC array programmed through three size registers: M, N and K at offsets
// 0x00, 0x08 and 0x10, START at 0x18, STATUS at 0x20. A multiply costs one
// cycle per 64 multiply-accumulates.
constexpr uint64_t kMacBase = 0x4000'0000;
constexpr uint64_t kStart = 0x18;
constexpr uint64_t kStatus = 0x20;

CommandDevice MacArray(Cycle access_cycles = 1) {
  return CommandDevice(
      "mac", kMacBase, access_cycles, /*params=*/3, kStart, kStatus,
      [](const std::vector<uint64_t>& p) { return p[0] * p[1] * p[2] / 64; });
}

Cycle LastWorkFinish(const RecordingSink& sink, std::string_view resource) {
  Cycle finish = 0;
  for (const Record& record : sink.records) {
    if (record.name == "work" && record.resource == resource) {
      finish = record.finish;
    }
  }
  return finish;
}

}  // namespace

TEST(CommandDevice, AnInitiatorProgramsItThroughTheAddressMap) {
  Memory<"DRAM"> dram(100);
  CommandDevice mac("mac", kMacBase, 1, /*params=*/1, /*start=*/0x08,
                    /*status=*/0x10,
                    [](const std::vector<uint64_t>& p) { return p[0]; });
  AddressMap map;
  map.Map(0, kMacBase, &dram);
  map.Map(kMacBase, kMacBase + 0x100, &mac);
  EventEngine engine;
  Initiator initiator(engine, 0);

  initiator.Issue(map, kMacBase, AccessType::kStore, 40);
  initiator.Issue(map, kMacBase + 0x08, AccessType::kStore, 1);
  initiator.Issue(map, kMacBase + 0x10, AccessType::kLoad);
  RunChecked(engine);

  EXPECT_EQ(initiator.BusyUntil(), 43u);

  CommandDevice core_mac("mac", kMacBase, 1, /*params=*/1, /*start=*/0x08,
                         /*status=*/0x10,
                         [](const std::vector<uint64_t>& p) { return p[0]; });
  AddressMap core_map;
  core_map.Map(0, kMacBase, &dram);
  core_map.Map(kMacBase, kMacBase + 0x100, &core_mac);
  EventEngine core_engine;
  CorePort core(core_engine, core_map);
  core.OnStore(kMacBase, 40);
  core.OnStore(kMacBase + 0x08, 1);
  core.OnLoad(kMacBase + 0x10);
  SyncChecked(core);

  EXPECT_EQ(core.Now(), initiator.BusyUntil());
}

TEST(CommandDevice, ReadmeAddressGraphFinishesAt4205Cycles) {
  Memory<"DRAM"> dram(100);
  Memory<"SPM"> spm(5);
  Cache<"L1", 64, 8, 64, LRUPolicy, 4> l1(&dram);
  constexpr uint64_t kBase = 0x9000'0000;
  CommandDevice mac(
      "mac", kBase, 1, 3, kStart, kStatus,
      [](const std::vector<uint64_t>& p) { return p[0] * p[1] * p[2] / 64; });
  AddressMap map;
  map.Map(0, 0x8000'0000, &l1);
  map.Map(0x8000'0000, 0x8001'0000, &spm);
  map.Map(kBase, kBase + 0x100, &mac);
  EventEngine engine;
  CorePort core(engine, map);

  core.OnLoad(0x1000);
  core.OnStore(kBase, 64);
  core.OnStore(kBase + 0x08, 64);
  core.OnStore(kBase + 0x10, 64);
  core.OnStore(kBase + kStart, 0);
  core.OnLoad(kBase + kStatus);
  SyncChecked(core);

  EXPECT_EQ(core.Now(), 4205u);
}

TEST(CorePort, CoreTimeIsItsInstructionsAndItsAccessesInOrder) {
  // A host that hooks only its load/store path and its instruction count gets
  // the core's time: compute at the CPI, and each access waiting for the one
  // before it.
  Memory<"DRAM"> dram(100);
  EventEngine engine;
  CorePort core_port(engine, dram, {.cpi = 1});

  core_port.OnInstructions(10);  // 0..10
  core_port.OnLoad(0x1000);      // 10..110
  core_port.OnInstructions(5);   // 110..115
  core_port.OnStore(0x2000, 7);  // 115..215
  SyncChecked(core_port);
  EXPECT_EQ(core_port.Now(), 215u);
}

TEST(CorePort, InstructionCostSaturatesBeforeMultiplicationWraps) {
  Memory<"DRAM"> dram(100);
  EventEngine engine;
  CorePort core_port(engine, dram, {.cpi = 2});

  core_port.OnInstructions(0);
  core_port.Sync();
  EXPECT_EQ(core_port.Now(), 0u);
  core_port.OnInstructions(3);
  core_port.Sync();
  EXPECT_EQ(core_port.Now(), 6u);

  core_port.OnInstructions(uint64_t{1} << 63);
  core_port.Sync();

  EXPECT_EQ(core_port.Now(), Cycle::Max());
}

TEST(CorePort, StartingADeviceCostsWhatItsParametersSay) {
  // Writing the size registers costs a core port transaction each; writing
  // START launches the multiply, costed from them. The core does not wait for
  // it.
  Memory<"DRAM"> dram(100);
  CommandDevice mac = MacArray(2);
  AddressMap map;
  map.Map(0, kMacBase, &dram);
  map.Map(kMacBase, kMacBase + 0x100, &mac);
  EventEngine engine;
  CorePort core_port(engine, map);

  core_port.OnStore(kMacBase + 0x00, 64);   // M: 0..2
  core_port.OnStore(kMacBase + 0x08, 64);   // N: 2..4
  core_port.OnStore(kMacBase + 0x10, 64);   // K: 4..6
  core_port.OnStore(kMacBase + kStart, 1);  // 6..8, launches 64*64*64/64 = 4096
  RecordingSink sink;
  SyncChecked(core_port, &sink);
  EXPECT_EQ(core_port.Now(), 8u);
  EXPECT_EQ(LastWorkFinish(sink, "mac.work"), 8u + 4096u);
}

TEST(CorePort, ReadingStatusWaitsForTheWorkNotForEachPoll) {
  // In the functional simulator the device finishes instantly, so the
  // software's polling loop reads "done" once. That one read waits for the
  // work, which is where a real core would have spun.
  Memory<"DRAM"> dram(100);
  CommandDevice mac = MacArray(2);
  AddressMap map;
  map.Map(0, kMacBase, &dram);
  map.Map(kMacBase, kMacBase + 0x100, &mac);
  EventEngine engine;
  CorePort core_port(engine, map);

  core_port.OnStore(kMacBase + 0x00, 64);
  core_port.OnStore(kMacBase + 0x08, 64);
  core_port.OnStore(kMacBase + 0x10, 64);
  core_port.OnStore(kMacBase + kStart, 1);  // work 8..4104
  core_port.OnInstructions(100);            // the core overlaps it: 8..108
  core_port.OnLoad(kMacBase + kStatus);     // waits: 4104..4106
  SyncChecked(core_port);
  EXPECT_EQ(core_port.Now(), 4106u);
}

TEST(CorePort, StatusStillWaitsWhenWorkFinishedBeyondTheLastSync) {
  Memory<"DRAM"> dram(100);
  CommandDevice mac("mac", kMacBase, 1, /*params=*/1, /*start=*/0x08,
                    /*status=*/0x10,
                    [](const std::vector<uint64_t>& p) { return p[0]; });
  AddressMap map;
  map.Map(0, kMacBase, &dram);
  map.Map(kMacBase, kMacBase + 0x100, &mac);
  EventEngine engine;
  CorePort core_port(engine, map);

  core_port.OnStore(kMacBase, 40);
  core_port.OnStore(kMacBase + 0x08, 1);
  CheckingSink checker;
  SyncChecked(core_port, checker);
  EXPECT_EQ(core_port.Now(), 2u);

  core_port.OnLoad(kMacBase + 0x10);
  SyncChecked(core_port, checker);
  ReportViolations(checker);
  EXPECT_EQ(core_port.Now(), 43u);
}

TEST(CorePort, ATransferOverlapsTheCoresComputeAndIsWaitedOnlyAtTheEnd) {
  // A DMA engine costed as a fixed setup plus bytes over bandwidth. The core
  // starts a 4 KB transfer, computes while it runs, then waits for it: the
  // total is the longer of the two, not their sum.
  Memory<"DRAM"> dram(100);
  CommandDevice dma("dma", 0x5000'0000, 1, /*params=*/1,
                    /*start=*/0x08, /*status=*/0x10,
                    [](const std::vector<uint64_t>& p) {
                      return 20 + (p[0] + 31) / 32;  // 20 + bytes / 32
                    });
  AddressMap map;
  map.Map(0, 0x5000'0000, &dram);
  map.Map(0x5000'0000, 0x5000'0100, &dma);
  EventEngine engine;
  CorePort core_port(engine, map);

  core_port.OnStore(0x5000'0000, 4096);  // size: 0..1
  core_port.OnStore(0x5000'0008, 1);     // start: 1..2, transfer 2..150
  core_port.OnInstructions(100);         // compute 2..102, while it runs
  core_port.OnLoad(0x5000'0010);         // wait: 150..151
  RecordingSink sink;
  SyncChecked(core_port, &sink);
  EXPECT_EQ(LastWorkFinish(sink, "dma.work"), 150u);
  EXPECT_EQ(core_port.Now(), 151u);  // not 2 + 148 + 100 + 1
}

TEST(CorePort, AnotherStartQueuesBehindWorkInProgress) {
  // The device serves one command at a time, in the order they were started.
  Memory<"DRAM"> dram(100);
  CommandDevice mac = MacArray();
  AddressMap map;
  map.Map(0, kMacBase, &dram);
  map.Map(kMacBase, kMacBase + 0x100, &mac);
  EventEngine engine;
  CorePort core_port(engine, map);
  for (int i = 0; i < 2; ++i) {
    core_port.OnStore(kMacBase + 0x00, 8);
    core_port.OnStore(kMacBase + 0x08, 8);
    core_port.OnStore(kMacBase + 0x10, 64);  // 8*8*64/64 = 64 cycles
    core_port.OnStore(kMacBase + kStart, 1);
  }
  RecordingSink sink;
  SyncChecked(core_port, &sink);
  // First: registers 0..4, work 4..68. Second: registers 4..8, queues, 68..132.
  EXPECT_EQ(LastWorkFinish(sink, "mac.work"), 132u);
}

TEST(CorePort, ALongRunHoldsOnlyWhatIsInFlight) {
  // Syncing as it goes, a host keeps the engine's state to what is in flight,
  // however long the program.
  Memory<"DRAM"> dram(100);
  Cache<"L1", 64, 4, 64, LRUPolicy, 2> l1(&dram);
  EventEngine engine;
  CorePort core_port(engine, l1);
  CheckingSink checker;
  std::size_t peak = 0;
  for (uint64_t i = 0; i < 20000; ++i) {
    core_port.OnInstructions(3);
    core_port.OnLoad((i * 64) % 8192);
    if (i % 100 == 99) {
      SyncChecked(core_port, checker);
      checker.Retire(
          core_port.Now());  // Sync just ran the engine to this horizon
      peak = std::max(peak, engine.InFlight());
    }
  }
  EXPECT_LT(peak, 10u);
  for (const auto& v : checker.Violations()) {
    ADD_FAILURE() << v.what;
  }
}
