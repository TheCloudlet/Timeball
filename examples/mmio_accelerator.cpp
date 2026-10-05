// Copyright 2025-2026 Yi-Ping Pan (Cloudlet)
//
// Timing a functional simulator by hooking only its load/store path.
//
// The "ISS" below stands in for yours: it executes a program and, as it does,
// reports the instructions it retires and the loads and stores it performs. The
// program runs a dispatch core that feeds a vector unit and a MAC array through
// memory-mapped registers — write the sizes, write START, poll STATUS — which
// is how software drives an accelerator. Timeball sees nothing else, and
// answers how long it took and where the time went.

#include <cassert>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "timeball/checking_sink.hpp"
#include "timeball/core_port.hpp"
#include "timeball/machine.hpp"
#include "timeball/record_query.hpp"
#include "timeball/timeball.hpp"

// Deliberate: a single self-contained example in which every name is
// timeball's; a dozen using-declarations would add nothing.
// NOLINTNEXTLINE(google-build-using-namespace)
using namespace timeball;

// ---- The cost table: the only place numbers live --------------------------
constexpr Cycle kDramCycles = 100;
constexpr uint64_t kVectorLanes = 8;
constexpr uint64_t kMacsPerCycle = 64;

// ---- The memory map --------------------------------------------------------
constexpr uint64_t kVpuBase = 0x4000'0000;
constexpr uint64_t kMacBase = 0x4000'1000;
// Both devices: parameters at 0x00, 0x08, 0x10; START 0x18; STATUS 0x20.
constexpr uint64_t kStart = 0x18;
constexpr uint64_t kStatus = 0x20;

struct Totals {
  Cycle waiting = 0;
  Cycle working = 0;
};

// ---- A stand-in functional simulator ---------------------------------------
// It computes nothing here; a real one would execute the program. What matters
// is where it calls the core_port: once per retired instruction batch, and once
// per load or store.
class FakeIss {
 public:
  explicit FakeIss(CorePort& core_port) : core_port_(&core_port) {}

  void Compute(uint64_t instructions) {
    core_port_->OnInstructions(instructions);
  }
  void Load(uint64_t addr) { core_port_->OnLoad(addr); }
  void Store(uint64_t addr, uint64_t value) {
    core_port_->OnStore(addr, value);
  }

  // What the driver code does: set the sizes, start, spin on STATUS. The
  // functional device finishes at once, so the spin reads "done" the first
  // time; the core port makes that one read wait for the work.
  void RunDevice(uint64_t base, uint64_t a, uint64_t b, uint64_t c) {
    Store(base + 0x00, a);
    Store(base + 0x08, b);
    Store(base + 0x10, c);
    Store(base + kStart, 1);
  }
  void WaitDevice(uint64_t base) { Load(base + kStatus); }

 private:
  CorePort* core_port_;
};

int main() {
  // Vector op over n elements: n / lanes cycles. Multiply of MxK by KxN: M*N*K
  // / (MACs per cycle).
  CommandDevice vpu("vpu", InitiatorId{1}, 2, 3, kStart, kStatus,
                    [](const std::vector<uint64_t>& p) {
                      return (p[0] + kVectorLanes - 1) / kVectorLanes;
                    });
  CommandDevice mac("mac", InitiatorId{2}, 2, 3, kStart, kStatus,
                    [](const std::vector<uint64_t>& p) {
                      return p[0] * p[1] * p[2] / kMacsPerCycle;
                    });
  Memory<"DRAM"> dram(kDramCycles);
  Cache<"L1", 64, 4, 64, LRUPolicy, 2> l1(&dram);
  AddressMap map;
  map.Map(0, kVpuBase, &l1);
  map.MapDevice(kVpuBase, kVpuBase + 0x100, &vpu);
  map.MapDevice(kMacBase, kMacBase + 0x100, &mac);
  Machine<"L1", 64, 4, 64, LRUPolicy, 2> machine(map);
  CorePort& core_port = machine.GetCorePort();
  FakeIss iss(core_port);
  auto summary =
      Group([](const Record& r) { return std::string(r.resource); }, Totals{},
            [](Totals t, const Record& r) {
              t.waiting += r.start - r.arrival;
              t.working += r.finish - r.start;
              return t;
            });
  // Checked here as a demonstration: attach it alongside any other sink, and
  // retire it in step with the engine's own horizon (README, "Checking the
  // engine").
  CheckingSink checker;

  // Eight 64x64x64 tiles. Per tile: load the operand descriptors, have the VPU
  // preprocess A, then the MAC multiply — the MAC of one tile overlapping the
  // core's work on the next.
  for (uint64_t tile = 0; tile < 8; ++tile) {
    iss.Compute(20);
    for (uint64_t line = 0; line < 4; ++line) {
      iss.Load(0x1000'0000 + tile * 0x1000 + line * 64);
    }
    iss.RunDevice(kVpuBase, uint64_t{64} * 64, 0, 0);
    iss.WaitDevice(kVpuBase);
    iss.WaitDevice(kMacBase);  // the previous tile's multiply
    iss.RunDevice(kMacBase, 64, 64, 64);
    BroadcastSink both(&summary, &checker);
    core_port.Sync(&both);  // a long program syncs as it goes
    checker.Retire(core_port.Now());
  }
  iss.WaitDevice(kMacBase);
  BroadcastSink both(&summary, &checker);
  core_port.Sync(&both);

  for (const auto& v : checker.Violations()) {
    std::fprintf(stderr, "violation: %s\n", v.what.c_str());
  }
  assert(checker.Ok());

  std::printf("total: %" PRIu64 " cycles\n\n", core_port.Now().value());
  std::printf("%-6s %10s %10s\n", "unit", "waiting", "working");
  for (const auto& [name, t] : summary.value()) {
    std::printf("%-6s %10" PRIu64 " %10" PRIu64 "\n", name.c_str(),
                t.waiting.value(), t.working.value());
  }
  return 0;
}
