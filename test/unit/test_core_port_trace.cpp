// Copyright 2025-2026 Yi-Ping Pan (Cloudlet)

#include <sstream>
#include <vector>

#include "checked_run.hpp"
#include "gtest/gtest.h"
#include "timeball/core_port_trace.hpp"
#include "timeball/timeball.hpp"

using namespace timeball;
using namespace timeball::test;

namespace {

constexpr uint64_t kMacBase = 0x4000'0000;
constexpr uint64_t kStart = 0x18;
constexpr uint64_t kStatus = 0x20;

CommandDevice MacArray() {
  return CommandDevice(
      "mac", InitiatorId{1}, 1, /*params=*/3, kStart, kStatus,
      [](const std::vector<uint64_t>& p) { return p[0] * p[1] * p[2] / 64; });
}

}  // namespace

TEST(CorePortTrace, RoundTripsThroughText) {
  const std::vector<CorePortEvent> events = {
      InstructionsRetired{10}, MemoryLoad{0x1000}, InstructionsRetired{5},
      MemoryStore{0x2000, 7}};

  std::stringstream buf;
  WriteCorePortTrace(buf, events);
  const auto read_back = ReadCorePortTrace(buf);
  ASSERT_TRUE(read_back.has_value());
  EXPECT_EQ(*read_back, events);
}

TEST(CorePortTrace, IgnoresBlankLinesAndComments) {
  std::stringstream in;
  in << "# a trace\n\nI 10\n\n# a comment mid-file\nL 0x40\n";
  const auto events = ReadCorePortTrace(in);
  ASSERT_TRUE(events.has_value());
  const std::vector<CorePortEvent> want = {InstructionsRetired{10},
                                           MemoryLoad{0x40}};
  EXPECT_EQ(*events, want);
}

TEST(CorePortTrace, AcceptsHexAndDecimalNumbers) {
  std::stringstream in;
  in << "L 0x1000\nS 4096 255\n";
  const auto events = ReadCorePortTrace(in);
  ASSERT_TRUE(events.has_value());
  const std::vector<CorePortEvent> want = {MemoryLoad{0x1000},
                                           MemoryStore{4096, 255}};
  EXPECT_EQ(*events, want);
}

TEST(CorePortTrace, RejectsAnUnknownEventLetter) {
  std::stringstream in;
  in << "X 1\n";
  EXPECT_FALSE(ReadCorePortTrace(in).has_value());
}

TEST(CorePortTrace, RejectsAMissingField) {
  std::stringstream in;
  in << "S 0x1000\n";  // store needs a value too
  EXPECT_FALSE(ReadCorePortTrace(in).has_value());
}

TEST(CorePortTrace, RejectsAnExtraField) {
  std::stringstream in;
  in << "L 0x1000 0x2000\n";
  EXPECT_FALSE(ReadCorePortTrace(in).has_value());
}

TEST(CorePortTrace, RejectsANonNumericField) {
  std::stringstream in;
  in << "I banana\n";
  EXPECT_FALSE(ReadCorePortTrace(in).has_value());
}

TEST(CorePortTrace, ApplyOneEventDispatchesLikeTheDirectCall) {
  Memory<"DRAM"> dram(100);
  EventEngine engine;
  CorePort core_port(engine, dram, {.cpi = 1});

  core_port.Apply(InstructionsRetired{10});  // 0..10
  core_port.Apply(MemoryLoad{0x1000});       // 10..110
  core_port.Apply(InstructionsRetired{5});   // 110..115
  core_port.Apply(MemoryStore{0x2000, 7});   // 115..215
  SyncChecked(core_port);
  EXPECT_EQ(core_port.Now(), 215u);
}

TEST(CorePortTrace, ApplyASequenceMatchesApplyingEachOneInOrder) {
  Memory<"DRAM"> dram(100);
  CommandDevice mac = MacArray();
  AddressMap map;
  map.Map(0, kMacBase, &dram);
  map.MapDevice(kMacBase, kMacBase + 0x100, &mac);
  EventEngine engine;
  CorePort core_port(engine, map);

  const std::vector<CorePortEvent> events = {
      MemoryStore{kMacBase + 0x00, 8}, MemoryStore{kMacBase + 0x08, 8},
      MemoryStore{kMacBase + 0x10, 64}, MemoryStore{kMacBase + kStart, 1}};
  core_port.Apply(events);
  RecordingSink sink;
  SyncChecked(core_port, &sink);
  // Registers 0..4, work 4..68 (8*8*64/64 = 64 cycles) — same as the
  // hand-called version in test_core_port.cpp's
  // AnotherStartQueuesBehindWorkInProgress.
  bool work_ends_at_68 = false;
  for (const Record& record : sink.records) {
    work_ends_at_68 |= record.name == "work" && record.finish == 68u;
  }
  EXPECT_TRUE(work_ends_at_68);
}

TEST(CorePortTrace, ReplayingARecordedTraceMatchesRunningItDirectly) {
  // The point of CorePortEvent: record a run, replay it against a *different*
  // machine, and get exactly what running the same calls directly would — no
  // functional simulator involved the second time.
  Memory<"DRAM"> dram(100);
  CommandDevice mac = MacArray();
  AddressMap map;
  map.Map(0, kMacBase, &dram);
  map.MapDevice(kMacBase, kMacBase + 0x100, &mac);
  EventEngine engine;
  CorePort core_port(engine, map, {.cpi = 2});

  core_port.OnStore(kMacBase + 0x00, 4);
  core_port.OnInstructions(20);
  core_port.OnStore(kMacBase + 0x08, 4);
  core_port.OnStore(kMacBase + 0x10, 64);
  core_port.OnStore(kMacBase + kStart, 1);
  core_port.OnLoad(kMacBase + kStatus);
  SyncChecked(core_port);
  const Cycle direct_now = core_port.Now();

  const std::vector<CorePortEvent> recorded = {
      MemoryStore{kMacBase + 0x00, 4},   InstructionsRetired{20},
      MemoryStore{kMacBase + 0x08, 4},   MemoryStore{kMacBase + 0x10, 64},
      MemoryStore{kMacBase + kStart, 1}, MemoryLoad{kMacBase + kStatus}};

  Memory<"DRAM2"> dram2(100);
  CommandDevice mac2 = MacArray();
  AddressMap map2;
  map2.Map(0, kMacBase, &dram2);
  map2.MapDevice(kMacBase, kMacBase + 0x100, &mac2);
  EventEngine engine2;
  CorePort core_port2(engine2, map2, {.cpi = 2});
  core_port2.Apply(recorded);
  SyncChecked(core_port2);

  EXPECT_EQ(core_port2.Now(), direct_now);
}
