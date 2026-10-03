// Copyright 2025-2026 Yi-Ping Pan (Cloudlet)

#include <vector>

#include "checked_run.hpp"
#include "gtest/gtest.h"
#include "timeball/address_map.hpp"
#include "timeball/machine.hpp"
#include "timeball/timeball.hpp"

using namespace timeball;
using namespace timeball::test;

namespace {

// A single-set, direct-mapped, 64-byte-line cache: tiny geometry, chosen so
// hit/miss is easy to force by hand rather than because it is realistic.
using TinyMachine = Machine<"L1", 1, 1, 64, LRUPolicy, 2>;

}  // namespace

TEST(Machine, WiresACompleteMachineACoreCanDriveThroughItsCorePort) {
  // Geometry (template args) and runtime numbers (MachineConfig) are all a
  // host supplies; everything else — MainMemory, Cache, EventEngine, CorePort —
  // is wired by Machine itself.
  TinyMachine machine({.dram_latency = 100, .core_port = {.cpi = 1}});
  CorePort& core_port = machine.GetCorePort();

  core_port.OnInstructions(10);  // 0..10
  core_port.OnLoad(0x1000);      // a miss: 10 + 2 (hit latency) + 100 (DRAM)
  SyncChecked(core_port);
  EXPECT_EQ(core_port.Now(), 112u);
}

TEST(Machine, DramLatencyIsARuntimeNumberNotBakedIntoTheTemplate) {
  // The same geometry, a different MachineConfig: proves the latency in
  // MachineConfig is what the miss actually costs, not a value Machine
  // hard-codes for its DRAM node.
  TinyMachine fast({.dram_latency = 10});
  TinyMachine slow({.dram_latency = 500});

  fast.GetCorePort().OnLoad(0x2000);
  SyncChecked(fast.GetCorePort());
  slow.GetCorePort().OnLoad(0x2000);
  SyncChecked(slow.GetCorePort());

  EXPECT_EQ(fast.GetCorePort().Now(), 12u);   // 2 (hit latency) + 10
  EXPECT_EQ(slow.GetCorePort().Now(), 502u);  // 2 (hit latency) + 500
}

TEST(Machine, AttachWiresAnMmioDeviceThroughToItsCorePort) {
  TinyMachine machine({.dram_latency = 100, .core_port = {.mmio_cycles = 1}});
  CorePort& core_port = machine.GetCorePort();
  CommandDevice mac("mac", /*params=*/1, /*start=*/0x08, /*status=*/0x10,
                    [](const std::vector<uint64_t>& p) { return p[0]; });
  machine.Attach(0x4000'0000, 0x100, mac);

  core_port.OnStore(0x4000'0000, 40);  // param: 0..1
  core_port.OnStore(0x4000'0008, 1);   // start: 1..2, work 2..42
  core_port.OnLoad(0x4000'0010);       // status: waits to 42..43
  SyncChecked(core_port);
  EXPECT_EQ(core_port.Now(), 43u);
  EXPECT_EQ(core_port.DeviceBusyUntil(mac), 42u);
}

TEST(Machine, MemoryIsTheNodeADirectAccessCanIssueInto) {
  // A host that also wants a plain Initiator (no CorePort involved for that
  // path) issues directly into Machine::Memory() — the same cache the CorePort
  // uses for its own non-MMIO loads and stores.
  TinyMachine machine({.dram_latency = 100});
  Initiator core(machine.Engine(), 0);
  core.Issue(machine.Memory(), 0x3000, AccessType::kLoad);
  RunChecked(machine.Engine());
  EXPECT_EQ(core.BusyUntil(), 102u);  // 2 (hit latency) + 100 (DRAM)
}

TEST(Machine, AHostSuppliedTopologyRoutesThroughAnAddressMap) {
  // More than one memory region: the host builds its own AddressMap and
  // caches, outside Machine, and hands over the entry node. Machine still
  // owns only the EventEngine and CorePort — the geometry TinyMachine was
  // instantiated with goes unused for this constructor.
  MainMemory<"Far"> far(100);
  Cache<"Region1", 1, 1, 64, LRUPolicy, 2> region1(&far);
  Scratchpad<"Region2"> region2(5);
  AddressMap map;
  map.Map(0x0000'0000, 0x8000'0000, &region1);
  map.Map(0x8000'0000, 0x8000'1000, &region2);

  TinyMachine machine(map, {.mmio_cycles = 1});
  CheckingSink checker;  // kept across both Syncs: each depends on the last
  machine.GetCorePort().OnLoad(0x1000);  // region1: miss, 2 + 100
  SyncChecked(machine.GetCorePort(), checker);
  EXPECT_EQ(machine.GetCorePort().Now(), 102u);

  machine.GetCorePort().OnLoad(0x8000'0000);  // region2: fixed 5
  SyncChecked(machine.GetCorePort(), checker);
  EXPECT_EQ(machine.GetCorePort().Now(), 107u);
  ReportViolations(checker);
}
