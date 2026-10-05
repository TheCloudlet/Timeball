// Copyright 2025-2026 Yi-Ping Pan (Cloudlet)
//
// Demonstrates timeball/core_port_trace.hpp: record a functional simulator's
// OnInstructions/OnLoad/OnStore calls into a CorePort once, write them to a
// trace file, then replay that one recording against two *different* sets of
// hardware parameters — with no functional simulator involved either time.

#include <cassert>
#include <cinttypes>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "timeball/core_port.hpp"
#include "timeball/core_port_trace.hpp"
#include "timeball/timeball.hpp"

// Deliberate, as in the other example: every name here is timeball's.
// NOLINTNEXTLINE(google-build-using-namespace)
using namespace timeball;

namespace {

constexpr uint64_t kMacBase = 0x4000'0000;
constexpr uint64_t kStart = 0x18;
constexpr uint64_t kStatus = 0x20;

// examples/mmio_accelerator.cpp's FakeIss, with its calls also recorded as
// CorePortEvents as they happen — a functional simulator needs no more than
// this to produce a replayable trace.
class RecordingIss {
 public:
  explicit RecordingIss(CorePort& core_port) : core_port_(&core_port) {}

  void Compute(uint64_t instructions) {
    core_port_->OnInstructions(instructions);
    events_.push_back(InstructionsRetired{instructions});
  }
  void Load(uint64_t addr) {
    core_port_->OnLoad(addr);
    events_.push_back(MemoryLoad{addr});
  }
  void Store(uint64_t addr, uint64_t value) {
    core_port_->OnStore(addr, value);
    events_.push_back(MemoryStore{addr, value});
  }

  void RunDevice(uint64_t base, uint64_t m, uint64_t n, uint64_t k) {
    Store(base + 0x00, m);
    Store(base + 0x08, n);
    Store(base + 0x10, k);
    Store(base + kStart, 1);
  }
  void WaitDevice(uint64_t base) { Load(base + kStatus); }

  [[nodiscard]] const std::vector<CorePortEvent>& Events() const {
    return events_;
  }

 private:
  CorePort* core_port_;
  std::vector<CorePortEvent> events_;
};

CommandDevice MacArray(uint64_t macs_per_cycle, Cycle access_cycles) {
  return CommandDevice("mac", InitiatorId{1}, access_cycles, 3, kStart, kStatus,
                       [macs_per_cycle](const std::vector<uint64_t>& p) {
                         return p[0] * p[1] * p[2] / macs_per_cycle;
                       });
}

// One hardware configuration a recorded trace can be replayed against.
struct Hardware {
  const char* label;
  uint64_t macs_per_cycle;
  Cycle register_cycles;
};

// Replays events against hw on a fresh engine and memory, and returns how long
// the recorded program took on that hardware. No functional simulator runs here
// — only the recorded calls.
Cycle Replay(const std::vector<CorePortEvent>& events, const Hardware& hw) {
  Memory<"DRAM"> dram(100);
  CommandDevice mac = MacArray(hw.macs_per_cycle, hw.register_cycles);
  AddressMap map;
  map.Map(0, kMacBase, &dram);
  map.MapDevice(kMacBase, kMacBase + 0x100, &mac);
  EventEngine engine;
  CorePort core_port(engine, map);
  core_port.Apply(events);
  core_port.Sync();
  return core_port.Now();
}

}  // namespace

int main() {
  // ---- Record the (fake) functional simulator's calls, once -------------
  Memory<"DRAM"> dram(100);
  CommandDevice mac = MacArray(/*macs_per_cycle=*/64, /*access_cycles=*/2);
  AddressMap map;
  map.Map(0, kMacBase, &dram);
  map.MapDevice(kMacBase, kMacBase + 0x100, &mac);
  EventEngine engine;
  CorePort core_port(engine, map);
  RecordingIss iss(core_port);

  iss.Compute(1000);
  iss.RunDevice(kMacBase, 64, 64, 64);
  iss.Compute(500);
  iss.WaitDevice(kMacBase);
  core_port.Sync();
  const Cycle direct_total = core_port.Now();

  // ---- Write the recording to a file, as if handed to someone else ------
  const std::string path = "/tmp/timeball_core_port_replay_example.trace";
  {
    std::ofstream out(path);
    WriteCorePortTrace(out, iss.Events());
  }

  // ---- Read the file back, replay against two different machines --------
  std::vector<CorePortEvent> replayed;
  {
    std::ifstream in(path);
    auto parsed = ReadCorePortTrace(in);
    assert(parsed.has_value() &&
           "the trace this example just wrote should parse");
    replayed = std::move(*parsed);
  }

  constexpr Hardware kFaster{"faster (mac 128/cyc, reg 1cyc)", 128, 1};
  constexpr Hardware kSlower{"slower (mac 32/cyc, reg 4cyc)", 32, 4};
  const Cycle faster_total = Replay(replayed, kFaster);
  const Cycle slower_total = Replay(replayed, kSlower);

  std::printf("direct  (mac 64/cyc, reg 2cyc):      %" PRIu64 " cycles\n",
              direct_total.value());
  std::printf("replay, %-32s %" PRIu64 " cycles\n", kFaster.label,
              faster_total.value());
  std::printf("replay, %-32s %" PRIu64 " cycles\n", kSlower.label,
              slower_total.value());

  // Same recording, two different replays: the numbers move with the hardware,
  // not with the (fixed, already-run) functional simulator.
  assert(faster_total < direct_total &&
         "faster hardware should finish the same recorded program sooner");
  assert(slower_total > direct_total &&
         "slower hardware should finish the same recorded program later");
  return 0;
}
