// Copyright 2025-2026 Yi-Ping Pan (Cloudlet)

#include <fstream>
#include <vector>

#include "checked_run.hpp"
#include "gtest/gtest.h"
#include "machines.hpp"
#include "timeball/core_port_trace.hpp"

using namespace timeball;
using namespace timeball::test;

namespace {

template <typename MachineT>
Cycle Finish(MachineT& machine, const std::vector<CorePortEvent>& events) {
  CorePort& core_port = machine.GetCorePort();
  core_port.Apply(events);
  SyncChecked(core_port);
  return core_port.Now();
}

// Two passes over two cache lines. The one-line cache misses on every
// line, both passes. The working-set cache hits on the second pass.
std::vector<CorePortEvent> TwoPasses() {
  std::vector<CorePortEvent> events;
  for (int pass = 0; pass < 2; ++pass) {
    events.push_back(MemoryLoad{0x0000});
    events.push_back(MemoryLoad{0x0040});
  }
  return events;
}

}  // namespace

TEST(TwoMachines, OneLineCacheFinishesLaterOnTheSameTranscript) {
  const std::vector<CorePortEvent> events = TwoPasses();
  OneLineMachine one_line(kReplayConfig);
  WorkingSetMachine working_set(kReplayConfig);
  const Cycle small = Finish(one_line, events);
  const Cycle large = Finish(working_set, events);
  EXPECT_GT(small, large);
}

TEST(TwoMachines, TheCommittedTranscriptKeepsThatOrder) {
  // The captured log is the whole proxy-kernel run, hundreds of thousands
  // of events. Sync directly, as the example does: the checking sink keeps
  // every completion, and that is the small transcript's job above.
  std::ifstream in(TIMEBALL_SPIKE_TRACE);
  ASSERT_TRUE(in.good());
  const auto events = ReadCorePortTrace(in);
  ASSERT_TRUE(events.has_value());

  OneLineMachine one_line(kReplayConfig);
  one_line.GetCorePort().Apply(*events);
  one_line.GetCorePort().Sync();
  WorkingSetMachine working_set(kReplayConfig);
  working_set.GetCorePort().Apply(*events);
  working_set.GetCorePort().Sync();
  EXPECT_GT(one_line.GetCorePort().Now(), working_set.GetCorePort().Now());
}
